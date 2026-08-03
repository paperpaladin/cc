#!/usr/bin/env python3
"""Generate a random C program that has exactly one correct answer.

This is the csmith idea without csmith: the generator is ours, which costs
nothing, because the generator is not the oracle. gcc and clang are, and they
were not told what we decided.

The whole difficulty is not emitting C -- it is emitting C that is *defined*.
A random expression is overwhelmingly likely to overflow a signed int, divide
by zero, or shift by a negative amount, and at that point gcc and clang are
entitled to disagree with each other and a difference proves nothing. Every
rule below exists to keep the program's value a fact rather than an opinion:

  - Arithmetic is done in `unsigned long`, where overflow is defined to wrap.
    Signed overflow is the single easiest way to generate undefined behaviour
    by accident, and it is invisible in the source.
  - A divisor is always `| 1`, so it is odd and therefore never zero.
  - A shift count is masked to 0..63, so it is always in range for a 64-bit
    operand.
  - Every variable is initialised before it is read.
  - The result is reduced mod 251 before becoming an exit status, because a
    process exit status is only eight bits and 251 is prime, so the low bits
    of a wrong answer are unlikely to collide with the right one.

Those rules are also the limits of what this finds: it cannot find a bug in
signed overflow behaviour, because it never generates any.

    python3 t/gen.py SEED > prog.c
"""
import random
import sys

BINOPS = ["+", "-", "*", "|", "&", "^"]

# Narrow types exist here to exercise the integer promotions, which is where
# the type system and the code generator have to agree. Keeping that defined
# needs care: "a + b" on two chars is INT arithmetic, and signed int overflow
# is undefined. So the values are bounded, and every sum, difference and
# product of two of them fits in an int with room to spare.
#
# A SIGNED type must be able to hold a NEGATIVE value, or the whole exercise
# is pointless. The first version masked everything to 0..127, so the sign
# bit was never set and a signed load was indistinguishable from an unsigned
# one -- planting "load a signed char with movzbl instead of movsbl" was
# caught by zero seeds out of a hundred. Signed types now land in -64..63,
# which still keeps a*b inside 4096.
#
# Each entry is (type, expression template for a value of that type).
NARROW = [
    ("signed char",    "(signed char)((%s & 0x7FUL) - 64)"),
    ("short",          "(short)((%s & 0x7FUL) - 64)"),
    ("int",            "(int)((%s & 0x7FUL) - 64)"),
    ("unsigned char",  "(unsigned char)(%s & 0xFFUL)"),
    ("unsigned short", "(unsigned short)(%s & 0xFFFUL)"),
    ("unsigned int",   "(unsigned int)(%s & 0xFFFFUL)"),
]


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.vars = []      # unsigned long, the accumulator type
        self.narrow = []    # (name, ctype) for the promotion cases
        self.arrays = []    # (name, length)
        self.structs = []
        self.enums = []    # names of enum-bit-field struct values
        self.lines = []

    # ---- expressions ------------------------------------------------------

    def expr(self, depth):
        r = self.r
        if depth <= 0 or (self.vars and r.random() < 0.3):
            if self.vars and r.random() < 0.7:
                return r.choice(self.vars)
            return "%dUL" % r.randint(0, 1 << 32)

        if self.narrow and r.random() < 0.12:
            return self.narrow_expr()
        if self.narrow and r.random() < 0.10:
            return self.narrow_call()
        if self.arrays and r.random() < 0.12:
            return self.array_expr()
        if self.structs and r.random() < 0.08:
            return self.struct_expr()

        k = r.random()
        if k < 0.55:
            return "(%s %s %s)" % (self.expr(depth - 1),
                                   r.choice(BINOPS),
                                   self.expr(depth - 1))
        if k < 0.70:
            # Odd divisor: never zero, and exercises a real division rather
            # than a shift the compiler can substitute for a power of two.
            return "(%s / ((%s) | 1UL))" % (self.expr(depth - 1), self.expr(depth - 1))
        if k < 0.80:
            return "(%s %% ((%s) | 1UL))" % (self.expr(depth - 1), self.expr(depth - 1))
        if k < 0.90:
            op = r.choice(["<<", ">>"])
            return "(%s %s ((%s) & 63UL))" % (self.expr(depth - 1), op, self.expr(depth - 1))
        if k < 0.95:
            return "(%s ? %s : %s)" % (self.cond(depth - 1),
                                       self.expr(depth - 1),
                                       self.expr(depth - 1))
        return "(~%s)" % self.expr(depth - 1)

    def cond(self, depth):
        r = self.r
        op = r.choice(["<", "<=", ">", ">=", "==", "!=", "&&", "||"])
        if op in ("&&", "||"):
            return "(%s %s %s)" % (self.cond(depth - 1), op, self.cond(depth - 1))
        return "(%s %s %s)" % (self.expr(depth - 1), op, self.expr(depth - 1))

    # ---- statements -------------------------------------------------------

    def decl(self):
        name = "v%d" % len(self.vars)
        self.lines.append("  unsigned long %s = %s;" % (name, self.expr(2)))
        self.vars.append(name)

    def observe(self, value):
        """Fold a value into the accumulator.

        Every construct does this, and it is the difference between a
        fuzzer and a program generator. Without it a construct can be
        emitted, be wrong, and never reach the exit status -- which is
        measurable: planting "pass a struct in one register instead of
        two" was caught by 1 seed in 100 when struct values only sometimes
        flowed to the result, because the other 99 computed a wrong number
        and then threw it away."""
        self.lines.append("  acc = acc * 31UL + (%s);" % value)

    def decl_narrow(self):
        ty, tmpl = self.r.choice(NARROW)
        name = "n%d" % len(self.narrow)
        self.lines.append("  %s %s = %s;" % (ty, name, tmpl % self.expr(1)))
        self.narrow.append((name, ty))
        # Observed twice, and both are deliberate.
        #
        # Directly, as a long rather than an unsigned long, so a
        # sign-extension bug on a negative value changes the answer.
        self.observe("(unsigned long)(long)%s" % name)
        # And through a call that widens it, because that is the only shape
        # that can see a missing conversion at the CALL site. Done here,
        # for every narrow variable, rather than left to a random statement
        # kind: when it was probabilistic it fired in 13 programs out of a
        # hundred and caught the planted defect in 1.
        self.observe("(unsigned long)widen(%s, %s)" % (name, name))

    def narrow_call(self):
        """Pass narrow values through a call. Nothing else here does, so the
        part of the prologue that stores a one- or two-byte argument into
        its slot was never executed -- planting "store it full width
        instead" was caught by zero seeds."""
        r = self.r
        a, _ = r.choice(self.narrow)
        b, _ = r.choice(self.narrow)
        return "(unsigned long)narrow(%s, %s)" % (a, b)

    def narrow_expr(self):
        """An expression over narrow variables, which is where the integer
        promotions actually happen: "a + b" on two chars is int arithmetic,
        and the result type decides whether the comparison after it is
        signed. The operands are bounded on assignment so this cannot
        overflow an int."""
        r = self.r
        a, _ = r.choice(self.narrow)
        b, _ = r.choice(self.narrow)
        op = r.choice(["+", "-", "*", "&", "|", "^"])
        return "(unsigned long)(%s %s %s)" % (a, op, b)

    def decl_array(self):
        n = self.r.randint(2, 5)
        name = "a%d" % len(self.arrays)
        vals = ", ".join("%dUL" % self.r.randint(0, 1 << 20) for _ in range(n))
        self.lines.append("  unsigned long %s[%d] = {%s};" % (name, n, vals))
        self.arrays.append((name, n))
        self.observe("%s[%d]" % (name, self.r.randrange(n)))

    def array_expr(self):
        name, n = self.r.choice(self.arrays)
        # The index is masked into range, so there is never an out-of-bounds
        # read -- which would be undefined and would make the references
        # disagree for reasons that say nothing about us.
        return "%s[(%s) %% %dUL]" % (name, self.expr(1), n)

    def decl_struct(self):
        name = "s%d" % len(self.structs)
        self.lines.append("  struct P %s; %s.a = %s; %s.b = %s;" %
                          (name, name, self.expr(1), name, self.expr(1)))
        self.structs.append(name)
        self.observe("fold(%s)" % name)

    def struct_expr(self):
        s = self.r.choice(self.structs)
        return "fold(%s)" % s

    def decl_enum_bitfield(self):
        """An enum-typed bit-field, which is where enum signedness becomes
        observable at all.

        C11 6.7.2.2p4 leaves the compatible type of an enum
        implementation-defined. gcc and clang both choose unsigned when
        no enumerator is negative, so a value with the top bit of the
        field set reads back positive; choose signed and it reads back
        negative. Nothing else in a program can see the difference,
        which is why this was a real miscompilation here and why no
        amount of ordinary expression fuzzing would have found it.

        Both enums are emitted: one all-positive, one with a negative
        member, because the rule is "derive the signedness from the
        enumerators" and a generator that only ever produced the first
        would pass against a compiler that simply made every enum
        unsigned."""
        name = "e%d" % len(self.enums)
        which = self.r.choice(("Epos", "Eneg"))
        val = self.r.choice((
            "SOME_POS", "SOME_POS + 1", "148", "150", "200", "255",
        )) if which == "Epos" else self.r.choice((
            "SOME_NEG", "SOME_NEG + 1", "0", "1", "60",
        ))
        self.lines.append("  struct B%s %s; %s.f = (%s);" %
                          (which[1:], name, name, val))
        self.enums.append(name)
        # Read back through the bit-field, which is the half that
        # matters: the store is easy and the load is where the sign
        # extension happens.
        self.observe("(unsigned long)(long)%s.f" % name)

    def enum_expr(self):
        e = self.r.choice(self.enums)
        return "(unsigned long)(long)%s.f" % e

    def switch_stmt(self, depth):
        """A switch, including fallthrough and a default.

        Nothing else here produces a jump table or a chain of
        comparisons, and the two are different code paths in most
        compilers. The selector is masked to a small range so the
        cases are reachable rather than decorative."""
        if not self.vars:
            self.decl()
        sel = "(int)((%s) %% 4UL)" % self.r.choice(self.vars)
        tgt = self.r.choice(self.vars)
        self.lines.append("  switch (%s) {" % sel)
        self.lines.append("  case 0:")
        self.lines.append("    %s = %s;" % (tgt, self.expr(2)))
        if self.r.random() < 0.5:
            self.lines.append("    /* falls through */")
        else:
            self.lines.append("    break;")
        self.lines.append("  case 1:")
        self.lines.append("  case 2:")
        self.lines.append("    %s = %s;" % (tgt, self.expr(2)))
        self.lines.append("    break;")
        self.lines.append("  default:")
        self.lines.append("    %s = %s;" % (tgt, self.expr(2)))
        self.lines.append("    break;")
        self.lines.append("  }")
        self.observe(tgt)

    def compound_assign(self):
        """Compound assignment and the increment operators.

        `x += e` is not `x = x + e` in the abstract machine (the operand
        is evaluated once) and it is not the same code either. All of
        these are on unsigned long, where every operation is defined and
        overflow wraps."""
        if not self.vars:
            self.decl()
        v = self.r.choice(self.vars)
        op = self.r.choice(("+=", "-=", "*=", "^=", "|=", "&="))
        self.lines.append("  %s %s (%s);" % (v, op, self.expr(2)))
        if self.r.random() < 0.5:
            form = self.r.choice(("%s++", "++%s", "%s--", "--%s"))
            self.observe(form % v)
        self.observe(v)

    def assign(self):
        if not self.vars:
            return self.decl()
        self.lines.append("  %s = %s;" % (self.r.choice(self.vars), self.expr(3)))

    def scoped(self, depth):
        """A nested block. Its declarations go out of scope with it, so they
        are dropped from the live set afterwards -- otherwise the final fold
        references names that no longer exist, and every compiler rejects
        the program rather than disagreeing about it."""
        marks = (len(self.vars), len(self.narrow), len(self.arrays),
                 len(self.structs), len(self.enums))
        self.block(depth)
        del self.vars[marks[0]:]
        del self.narrow[marks[1]:]
        del self.arrays[marks[2]:]
        del self.structs[marks[3]:]
        del self.enums[marks[4]:]

    def if_stmt(self, depth):
        self.lines.append("  if (%s) {" % self.cond(2))
        self.scoped(depth - 1)
        if self.r.random() < 0.5:
            self.lines.append("  } else {")
            self.scoped(depth - 1)
        self.lines.append("  }")

    def loop(self, depth):
        if not self.vars:
            self.decl()
        i = "i%d" % self.r.randint(0, 999)
        n = self.r.randint(1, 6)
        tgt = self.r.choice(self.vars)
        self.lines.append("  for (int %s = 0; %s < %d; %s++) {" % (i, i, n, i))
        self.lines.append("    %s = %s;" % (tgt, self.expr(2)))
        self.scoped(depth - 1)
        self.lines.append("  }")

    def call(self):
        # A function whose body is a fold over its parameters. Exercises the
        # calling convention with values the caller cannot constant-fold away.
        if len(self.vars) < 2:
            self.decl(); self.decl()
        a, b = self.r.sample(self.vars, 2)
        self.lines.append("  %s = mix(%s, %s);" % (self.r.choice(self.vars), a, b))
        # Wide values into narrow parameters, which is the only shape that
        # needs a conversion AT THE CALL. Passing an already-narrow variable
        # makes the conversion a no-op, and removing it was caught by zero
        # seeds.
        self.observe("(unsigned long)narrow(%s, %s)" % (a, b))

    def widen_call(self):
        """Narrow values into wide parameters. This is the only shape that
        can see a missing conversion AT THE CALL: a signed char in a
        register says nothing about its top 56 bits, and the callee reading
        a long is far too late to fix it. The opposite direction cannot,
        because the callee truncates into its own slot and is right either
        way."""
        a, _ = self.r.choice(self.narrow)
        b, _ = self.r.choice(self.narrow)
        self.observe("(unsigned long)widen(%s, %s)" % (a, b))

    def block(self, depth):
        for _ in range(self.r.randint(1, 4)):
            k = self.r.random()
            if k < 0.26 or not self.vars:
                self.decl()
            elif k < 0.42:
                self.assign()
            elif k < 0.52:
                self.decl_narrow()
            elif k < 0.60:
                self.decl_array()
            elif k < 0.58:
                self.decl_struct()
            elif k < 0.66:
                self.decl_enum_bitfield()
            elif k < 0.74:
                self.compound_assign()
            elif k < 0.80 and depth > 0:
                self.if_stmt(depth)
            elif k < 0.85 and depth > 0:
                self.loop(depth)
            elif k < 0.92 and depth > 0:
                self.switch_stmt(depth)
            elif k < 0.96 and self.narrow:
                self.widen_call()
            else:
                self.call()

    def program(self):
        self.lines.append("struct P { unsigned long a, b; };")
        # Two enums and two bit-fields of enum type. Epos has no
        # negative enumerator so its compatible type is unsigned, and a
        # value with bit 7 set reads back positive; Eneg has one, so it
        # is signed and the same bit pattern reads back negative. That
        # difference is the only way an enum's signedness is observable,
        # and getting it wrong was a real miscompilation here.
        self.lines.append("enum Epos { SOME_POS = 148, NEXT_POS };")
        self.lines.append("enum Eneg { SOME_NEG = -1, NEXT_NEG = 148 };")
        self.lines.append("struct Bpos { enum Epos f : 8; unsigned g : 1; };")
        self.lines.append("struct Bneg { enum Eneg f : 8; unsigned g : 1; };")
        self.lines.append("unsigned long mix(unsigned long a, unsigned long b) {")
        self.lines.append("  return (a * 3UL) ^ (b + 7UL) ^ (a >> 3UL);")
        self.lines.append("}")
        # A 16-byte struct is two integer registers by the SysV rules, so
        # calling this exercises the classification rather than a memcpy.
        # Narrow parameters, so the prologue path that stores a one- or
        # two-byte argument is reached. The body sign-extends by returning
        # a long, which is where a wrong load shows up.
        self.lines.append("long narrow(signed char a, unsigned short b) {")
        self.lines.append("  return (long)a * 3L + (long)b;")
        self.lines.append("}")
        # And the other direction. Passing a WIDE value to a narrow
        # parameter cannot test the conversion at the call, because the
        # callee truncates into its own one-byte slot and gets the right
        # answer either way -- the same vacuity as g(300) in the README.
        # Only widening can see it: a signed char in a register says
        # nothing about its top 56 bits, and the callee is too late.
        self.lines.append("long widen(long a, long b) {")
        self.lines.append("  return a * 7L - b;")
        self.lines.append("}")
        self.lines.append("unsigned long fold(struct P p) {")
        self.lines.append("  return (p.a ^ (p.b * 5UL)) + (p.a >> 7UL);")
        self.lines.append("}")
        self.lines.append("int main(void) {")
        self.lines.append("  unsigned long acc = 0UL;")
        # Several top-level blocks rather than one. With a single block
        # of one to four statements, a construct chosen 8% of the time
        # appeared in about one program in thirty, which is not fuzzing
        # it, it is occasionally mentioning it. The census in t/fuzz.sh
        # is what says whether this is enough.
        for _ in range(self.r.randint(3, 6)):
            self.block(3)
        total = " ^ ".join(self.vars) if self.vars else "0UL"
        self.lines.append("  unsigned long result = acc ^ (%s);" % total)
        # 251 is prime, so a wrong answer is unlikely to agree in the low
        # eight bits by coincidence.
        self.lines.append("  return (int)(result % 251UL);")
        self.lines.append("}")
        return "\n".join(self.lines) + "\n"


def main():
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    sys.stdout.write(Gen(seed).program())


if __name__ == "__main__":
    main()
