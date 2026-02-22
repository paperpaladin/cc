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


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.vars = []
        self.lines = []

    # ---- expressions ------------------------------------------------------

    def expr(self, depth):
        r = self.r
        if depth <= 0 or (self.vars and r.random() < 0.3):
            if self.vars and r.random() < 0.7:
                return r.choice(self.vars)
            return "%dUL" % r.randint(0, 1 << 32)

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

    def assign(self):
        if not self.vars:
            return self.decl()
        self.lines.append("  %s = %s;" % (self.r.choice(self.vars), self.expr(3)))

    def scoped(self, depth):
        """A nested block. Its declarations go out of scope with it, so they
        are dropped from the live set afterwards -- otherwise the final fold
        references names that no longer exist, and every compiler rejects
        the program rather than disagreeing about it."""
        mark = len(self.vars)
        self.block(depth)
        del self.vars[mark:]

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

    def block(self, depth):
        for _ in range(self.r.randint(1, 3)):
            k = self.r.random()
            if k < 0.35 or not self.vars:
                self.decl()
            elif k < 0.60:
                self.assign()
            elif k < 0.75 and depth > 0:
                self.if_stmt(depth)
            elif k < 0.88 and depth > 0:
                self.loop(depth)
            else:
                self.call()

    def program(self):
        self.lines.append("unsigned long mix(unsigned long a, unsigned long b) {")
        self.lines.append("  return (a * 3UL) ^ (b + 7UL) ^ (a >> 3UL);")
        self.lines.append("}")
        self.lines.append("int main(void) {")
        self.block(3)
        total = " ^ ".join(self.vars) if self.vars else "0UL"
        self.lines.append("  unsigned long result = %s;" % total)
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
