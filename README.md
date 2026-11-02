# cc

A small C compiler for x86-64 Linux. Tokenizes, parses, and emits System V
assembly; `gcc` does the assembling and linking.

```
$ make
$ ./cc examples/fib.c -o fib && ./fib; echo $?
55
```

## Why this exists

A compiler is the only project I know of where the test oracle is completely
outside the author's control. Either `gcc` and `clang` agree with your output or
they don't, and no amount of confidence changes the exit code. Everything here
is organised around that. The real test suite contains no hand-written
expected value anywhere in it.

That turned out to matter on the first run. Two cases failed, I assumed two
codegen bugs, and both were wrong expectations I had computed by hand
(`-17 % 5` is `-2`, not `-3`). The differential harness would have caught them
without the detour.

## It compiles itself

```
$ make self-host
ok   stage 1: cc compiles cc.c
ok   stage 2: cc2 compiles cc.c
ok   the fixpoint: cc2 and cc3 are byte-identical
ok   cc2 passes t/run.sh  (408 passed, 0 failed)
ok   cc2 passes t/cpp.sh  (70 passed, 0 failed)
ok   cc2 builds an ordinary program through real headers

6 passed, 0 failed
```

The fixpoint is this project's hard invariant, and what it proves is
narrower than it first looks. It is a *fixed point of itself*.
Whatever `cc2` does differently from `cc`, it does consistently enough to
reproduce itself byte for byte. A miscompilation on
any path `cc.c` exercises shows up as `cc3` differing from `cc2`.

It does not prove `cc2` is correct, and this is not a theoretical
caveat. At the first green `cmp cc2 cc3` here, `cc2` still failed 15 cases
of `t/run.sh`. The fixpoint said *stable* and the suite said *wrong*, and
the suite was right. A compiler that miscompiles itself self-consistently
reaches a fixpoint perfectly happily. So `make self-host` runs the whole
suite against the stage-2 compiler as well, and it is the second half that
has teeth.

## Status

```
$ make test        # exit-status assertions; two harnesses, two summaries
408 passed, 0 failed
70 passed, 0 failed

$ make diff-test   # the oracle that matters
323 agree with gcc+clang, 0 disagree, 7 skipped
11 multi-line case(s) in t/run.sh are not differential-tested

$ make conformance # 220 programs nobody here wrote
c-testsuite: 195 of 220 pass (88%)
  no regressions against t/cts-baseline.txt
  by kind:
       25 refused

$ make overread    # the sanitizer can see the end of the source buffer
ok   the planted read past the file buffer is reported
```

Two suites, two summaries, deliberately never added together. `t/run.sh`
asserts exit statuses and `t/cpp.sh` asserts preprocessor output, and they
answer different questions, so 408 and 70 are quoted apart and 478 appears
nowhere. The collection's audit knows this repository reports them that
way.

`make diff-test` was broken for fifteen days and nobody noticed, because
it is not part of `make test`. Three enum bitfield cases were added across
two lines each using `check` rather than `check_src`, and the extraction
that builds this harness's subject reads one line at a time, so the first
of them reached it as an unterminated quote and the sourced file stopped
parsing. The script ended there, before the floor check that exists for
exactly this, and printed one `Syntax error` line and exit 2. It now runs
`sh -n` over the extract first and names the offending line numbers.

The 7 skips are named in the output, which they were not until recently. Two
are `printf`/`strlen` without a declaration, clang 19 defaults to C23, where
implicit function declarations are an error, so it will not build them as a
reference. The other five are every `va_list` case, including the
`vsprintf` one this README calls the real oracle: `va_list` is a cc builtin,
so gcc rejects those sources without `<stdarg.h>`. So the variadic ABI is not differential-tested at all, and that was invisible while the
skip was silent.

### What works

Types: `void`, `_Bool`, `char`, `short`, `int`, `long`, their `unsigned`
forms, pointers, arrays (multidimensional, with the `a[i] == *(a+i)` identity),
`struct`, `union`, `enum`, and `typedef`. Struct layout computes real alignment
and padding. Unions overlay at offset 0. Incomplete types are tracked, so a
forward-declared `struct` can be referenced through a pointer and completed later.

Expressions, full operator precedence, short-circuit `&&`/`||`, ternary,
comma, compound assignment, pre/post increment and decrement, `sizeof` over both
expressions and type names, casts with correct signed/unsigned width conversion,
member access through `.` and `->`, struct assignment, and constant folding for
array dimensions and enum values.

Statements: `if`/`else`, `for`, `while`, `do`, `break`, `continue`,
`switch`/`case`/`default` with fallthrough, `goto` and labels, and real block
scoping so an inner declaration shadows an outer one.

Initialisers: `int a[3] = {1,2,3}`, `int a[] = {1,2}` with inferred length,
`char s[] = "abc"` (4 bytes, the NUL is part of the object), `struct P p = {4,5}`,
and nested forms: arrays of structs, structs containing arrays, 2D arrays.
Omitted elements are zero-initialised explicitly, because a local lives on an
uninitialised stack.

Designators, `[2] = v` and `.member = v`, in both static and local
initialisers, mixed freely with positional elements, which continue from
wherever the last designator left the cursor. An unsized array takes its
length from the highest index reached rather than from the comma count, so
`int a[] = {5, [2] = 2, 3}` has four elements. A local list holding a
designator is zeroed before it is written rather than after, because a
designator can go backwards and the trailing fill would undo it.

Calls, definitions and calls up to 6 arguments, recursion, and calls into
libc that get the ABI right.

Structs by value, up to 16 bytes, passed in one or two integer
registers, returned in `rax:rdx`, checked in both directions across a `gcc`
boundary. The full SysV classification walks every field because a `double`
lands in a different register file. This compiler has none, so the
algorithm collapses to a size check, with the one hole that `float` and
`double` are *declarable* here, so a struct containing one is refused,
not quietly passed in the wrong place.

Function pointers, declaring, assigning, passing, returning through,
arrays of them, and calling through one from a struct member. Needed
recursive declarator parsing, because `int (*fp)(int)` cannot be read in one
pass: the suffix after the closing paren describes what the thing inside the
parens points *at*.

Bitfields, packing, `:0` to break a storage unit, unnamed padding
fields, signed and unsigned, up to the full width of the underlying type.
Layout matches gcc on every case in the suite. A field that will not fit
whole in the open unit starts a new one instead of straddling.

Variadic definitions: `int sum(int n, ...)` with the SysV register
save area, plus `va_list`, `va_start`, `va_arg`, `va_end` and `va_copy`
predefined, so none of it needs a header. The real `<stdarg.h>` is GNU C
from its first line. Avoiding that dependency is the point.

Global initialisers, scalars, arrays, nested arrays, structs, strings
into `char` arrays, and addresses: `char *p = "abc"`, `int *p = &v`, `int *p
= a + 2`, and tables like `char *K[] = {"a","bb",0}`. An address is not a
byte pattern, so those become relocations the linker fills in. Initialised
`static` locals work too. Uninitialised globals go in `.bss`, initialised
ones in `.data`, string literals in `.rodata`.

`static` and `extern`, internal linkage on globals and functions,
`extern` declarations that reference another translation unit without
defining anything, and `static` locals, which have block scope and static
storage and so are emitted as globals under a generated name.

Function prototypes, a declaration gives a call its return type, so
`sizeof(f())` is right and a returned pointer can be dereferenced. Arguments
are converted to their parameter types at the call, and the argument count is
checked. `f(void)` and `f()` are kept apart. The first promises no arguments
and is checked, the second promises nothing and is not. A call to an
undeclared function still works and still assumes it returns `long`.

Preprocessor: `#include` (both spellings, with `-I`), object and
function-like macros, `#undef`, the full `#if`/`#ifdef`/`#ifndef`/`#elif`/
`#else`/`#endif` family with constant-expression evaluation and `defined`,
`#` stringification, `##` pasting, variadic macros including the GNU
`, ## __VA_ARGS__` comma deletion and the GNU `args...` naming, `__FILE__`,
`__LINE__`, `#error`, `#include_next`, `#pragma push_macro` and `pop_macro`,
and `-D`/`-U` on the command line. `-E` prints the result.

`L'x'`, `u'x'` and `U'x'` character constants, which are not decoration:
glibc settles `WCHAR_MAX` in `<bits/wchar.h>` with `#elif L'\0' - 1 > 0`, so
a tokenizer that cannot lex that cannot read `<wchar.h>` at all, and the
symptom is an undefined identifier `L` pointing into a system header. The
value is not truncated to a signed char the way a plain `'x'` is, because
`wchar_t` is a 32-bit int here.

### What does not

**The preprocessor does not have `_Pragma`, `#line`, `__DATE__`, `__TIME__`
or `__COUNTER__`**, and it does not police the constraint that a
redefinition be identical: a second `#define` silently replaces the first.
(This used to list `#include_next` here while the Real headers section
below explained how `#include_next` works. One of the two was wrong for as
long as both were in the file.)
A stray backslash outside a string literal is rejected, not carried
along as a preprocessing token, which is legal C that real preprocessors
accept. Nothing here needs it, and the tokenizer is simpler without it.

No floating-point arithmetic. `float`, `double` and `long double` are
declarable: they parse, size correctly at 4, 8 and 16, and appear in
declarations, enough for real headers, but producing a value of
one is an error, not a silent miscompilation. No `va_arg` of a
struct.

A floating *constant* follows the same line. `1.5` is one token and is
refused where it is used, not where it is lexed. That ordering is not a
detail: tokenizing runs over the whole file before any `#if` is evaluated,
so refusing at the lexer rejects a header for a literal nothing ever
reaches. `<math.h>` defines `HUGE_VAL` as `1e10000` and `<features.h>` has
`4.1` inside a `#warning` message, and with the refusal moved into the
tokenizer 7 of the 12 headers this compiler is pointed at stop compiling.
Measured, not reasoned about.

Until recently `100.0` lexed as `100`, then `.`, then `0`, so a program
with a double in it was turned away with "`.` applied to something that is
not a struct or union". Five corpus cases got that message. It was a true
statement about the tokens and no help at all.

`const`, `volatile` and `restrict` parse and are then ignored. There is no
const-checking here, and saying so is better than implying there is.

A call may pass at most six arguments, because there is no support for
arguments on the stack. A *definition* may receive more than six through
`va_arg`, which is how that path gets tested at all, gcc makes the call.
The same gap is why a struct larger than 16 bytes cannot be passed by
value. That is the MEMORY class, and it needs the thing that is missing.
Every one of these is refused by name, and `t/run.sh` asserts the refusal
with `unimpl()`, gcc must accept the program and we must reject it
cleanly, because the failure mode of a missing feature is not a missing
error message but quietly wrong code for something that looked close enough.

It compiles itself. See the top of this file.

## Design

One file. Splitting a 5,108-line compiler into `lex.c`/`parse.c`/`codegen.c`
buys nothing while it still fits in a single screen of `ctags`, and a single
translation unit is easier to self-host later.

Three deliberate corners, each marked `shortcut:` in the source with its
ceiling:

- Stack-machine codegen, no register allocator. Every binary operator
  pushes and pops its operands. Two cases avoid it. A leaf right operand
  goes straight into `%rdi`, and a call whose arguments are all leaves
  loads each one directly into its own register, which is worth about 3%
  and is not a fix. Compiling this compiler's own source emitted 37,646
  instructions against `gcc -O0`'s 15,925: a 2.4x gap, measured on a
  frozen 4,677-line snapshot of `cc.c` that is **not committed**, so the
  figure cannot be re-run from this repo and `cc.c` is 5,108 lines now.
  Closing the gap means a real register allocator (PLAN item 22).

  Measuring this needs a *frozen* input. That is not obvious until it
  bites. The first numbers here compared the compiler's output on `cc.c`
  before and after a change to `cc.c` itself, so a 2.4% improvement read as
  a 0.7% regression. That benchmark is now a fixed snapshot.
- Nothing is freed. A compiler is a short-lived process; `exit` reclaims.
- `gcc` assembles and links. Writing an assembler is a different project.

### Why the preprocessor keeps a set per token

The only genuinely hard thing in a preprocessor is stopping.

```c
#define f(x) f(x)
f(1)              // must become f(1), and then stop
```

A single "am I currently expanding f?" flag gets this wrong, because the same
identifier can be live in one place and finished in another within a single
expansion. C 6.10.3.4 instead paints each *token*: everything produced by
expanding `f` carries `f` in its hide set and can never be expanded by `f`
again. Function-like macros intersect the hide sets of the name and the
closing paren, because the tokens between them may have arrived from different
expansions and only what both ends agree is painted may carry forward.

The payoff is that mutual recursion (`#define a b` / `#define b a`) and
indirect self-reference fall out of the same rule instead of needing their
own special case. With the hide-set test removed, `f(1)` does not terminate,
see the mutation table below.

### `-E` output has to read back

Expansion puts tokens next to each other that were never adjacent in the
source, and some of those pairs lex differently when printed touching. `2` and
`+` are the dangerous case. A pp-number may contain an exponent, so `2+` is one
token to a conforming lexer. The printer therefore inserts a space wherever two
adjacent tokens could glue, and the suite asserts the resulting guarantee
directly, running `-E` over its own output must produce that output again.

Worth noting as a calibration: C99 6.10.3.5's printed example shows `2+(3,4)`,
and gcc, clang and this compiler all emit `2 +(3,4)`. The implementations are
right and the standard's illustrative text is loose.

## Tests

`t/run.sh` holds the cases and asserts exit statuses. `t/differential.sh`
re-runs the same cases but throws the expectations away and compares against gcc
and clang instead, if the two reference compilers disagree with each other the
case is skipped as undefined behaviour, not counted against us.

Cases live in exactly one place. `differential.sh` greps them out of `run.sh`,
not keeping a second copy in sync.

`t/addcase.sh` is a tool rather than a suite, so it is deliberately not
reachable from the Makefile. It adds a case with its expected value
**taken from gcc**:

```
$ ./t/addcase.sh 'int main() { int a[]={1,2,3}; return sizeof(a); }'
added: expected 12 (gcc and clang agree)
```

Five of my hand-computed expectations have been wrong across this project, each
one costing a detour through "is this a codegen bug?" The compiler was right every
time. So I stopped guessing, gcc already knows, and if gcc and clang disagree the
program relies on undefined behaviour and has no business in the suite.

`t/cpp.sh` is the same idea aimed at the preprocessor: 70 cases, every
expectation produced by **gcc -E -P and clang -E -P**, both of which must
agree with us. It includes C99 6.10.3.5 examples 3 and 5 verbatim, which are
the published torture tests for rescanning, `#`, `##` and variadic macros.

```
$ make test
408 passed, 0 failed
70 passed, 0 failed

$ make test-asan        # the same suite, watched
408 passed, 0 failed
70 passed, 0 failed
```

`make test-asan` rebuilds the compiler with AddressSanitizer and
UndefinedBehaviorSanitizer and runs everything again. Two settings in that
target are load-bearing, not tidy:

`ASAN_OPTIONS=exitcode=86`, because a sanitizer abort exits 1 and so does a
correct refusal. Without separating them, `refuse()` scores a crash as a
pass and the entire sanitizer build quietly stops meaning anything. That
exact collision is a bug already recorded in a sibling project, so it was
cheaper to assume it than discover it.

`detect_leaks=0`, because this compiler frees nothing on purpose and
LeakSanitizer is right every single time it complains. With it on, every case
fails since every run ends holding its whole AST. Measured today: `1 passed,
407 failed`, and the 1 is the interesting part, because this used to say
"0 passed, 325 failed" and call the number the case count. It is not: one
case gets through. All of them saying the same known thing is the fastest
way to stop reading output, which is the reason the option is off.

### What the sanitizer could not see

`make test-asan` printed a clean run for as long as it existed, over an
area smaller than it looked. A buffer larger than its data turns the
sanitizer off: a read one byte past the end of the data is still inside a
live allocation, and there is nothing for AddressSanitizer to report.

The buffer here is the one holding the program being compiled and every
header it pulls in, with a separate `f->len` beside it, which is the most
untrusted input a compiler has. Two things left slack in it. `read_file`
allocates the file size plus two, for a newline phase 1 may have to append
and for a NUL, and usually only one of the two is used. And `splice_lines`
DELETES a byte for every line continuation without touching the
allocation, which is the harder half, because no allocation size can see
it and glibc's headers use continuations constantly.

`new_file` trims the text to its length now, and every buffer that is ever
tokenized passes through `new_file`: both `read_file` call sites, `-D` on
the command line, `##` pastes, `#` stringifications, `__FILE__`, `__LINE__`
and the built-in definitions. One `realloc` rather than eight.

```
$ make overread
ok   the planted read past the file buffer is reported
     READ of size 1 at 0x50300000005f thread T0
         #0 0x5610a3260e7d in new_file cc.c:120
```

`-DCC_OVERREAD` compiles a deliberate read of `f->text[f->len + 1]` and
the target fails unless the sanitizer reports it. Exit 86, because 0 is
success, 1 is a clean refusal, 2 is a usage error and 127 is a failed
exec, and a sanitizer report that can be mistaken for any of those is how
a whole sanitizer build stops meaning anything.

This target is the only witness, and that is measured rather than assumed.
With the `realloc` reverted, `make overread` fails having had nothing
reported while `t/run.sh` prints 408 passed, 0 failed and `make test-asan`
prints 408 and 70 passed.

It also passed for the wrong reason first, which is worth more than the
fix. The probe fired on every buffer, and several of the synthesized ones
are already allocated to the exact byte, so with the trim reverted the
report arrived from `<built-in>` and the target went green. It now probes
only files whose name is not in angle brackets. A gate watched to fail is
not the same as a gate watched to fail for the right reason.

One detail of the target, because it is easy to undo by accident: the
probe file carries a line continuation deliberately. Without one only
`read_file`'s two spare bytes are in play, and the invisible half of the
shape, the byte `splice_lines` deletes, goes ungated.

What is left, so the next sweep starts from a list rather than from
memory. `static_local_name` still asks for `strlen(name) + 16` bytes to
hold a label about twelve bytes long. The tokenizer's string literal
buffer is a `char[4096]` with an explicit length check and an error. Seven
more fixed stack arrays, `char[32]` and `char[24]`, hold generated labels
and names written by `snprintf`. None of them holds input and none has a
gate over it. That is a decision rather than an oversight, and it is the
list to start from if one of those ever holds something a program chose.

### Watching that suite fail

A green suite means nothing until it has been seen to go red, so seven defects
were planted in the preprocessor one at a time:

| planted defect | result |
|---|---|
| hide set never consulted | caught, `f(1)` does not terminate |
| `#` does not escape quotes or backslashes | caught, 1 case |
| the operand of `##` is expanded first | caught, 2 cases |
| no space inserted at a paste hazard | caught by the idempotence check |
| an expansion keeps its definition site | caught, `__LINE__` reports the `#define` |
| an argument is substituted unexpanded | caught, 2 cases |
| `#elif` is evaluated after a branch was taken | caught, 1 case |

All seven. The fourth is the one worth having. It fails nothing about what the
tokens *are*, only that printing them loses information, and it is caught only
because the suite re-reads its own output instead of trusting it.

The same was done for prototypes, and there the exercise paid for itself
immediately. Seven defects, six caught, and the survivor was **"arguments are
not converted at the call"**, which the suite was supposed to cover.

The test was `g(300)` against `int g(char c)`, expecting 44. It returns 44
whether or not the call converts anything, because the callee stores its
parameter into a one-byte slot and truncates it there. The assertion was true
and meaningless. Narrowing cannot test a call-site conversion; only widening
can, because an `int` in a register says nothing about its top 32 bits and the
callee is too late to fix it:

```c
int f(long x){ return x == -1; }
int main(){ int i = -1; return f(i); }   // 1 only if the call sign-extends
```

With that case in, the mutant dies.

### Random programs, judged by two compilers

```
$ make fuzz FROM=1 TO=200
200 agree, 0 disagree, 0 skipped (seeds 1..200)
reached: 116 enum bit-field, 122 switch, 136 compound assign, 200 struct by value, 200 narrow call, 100 loop (of 200 seeds)

$ make fuzz FROM=1 TO=1200
1200 agree, 0 disagree, 0 skipped (seeds 1..1200)
reached: 707 enum bit-field, 750 switch, 798 compound assign, 1200 struct by value, 1200 narrow call, 560 loop (of 1200 seeds)
```

The 1200-seed run is the widest this has been taken and it found nothing.
That is a result and not a reassurance: it bounds what this generator can
see, and the bound is in the paragraphs below. Everything fixed in this
repository recently came from the corpus in the next section, not from
here.

That second line is the point of the first. 400 seeds agreeing is
either evidence the compiler is right or evidence the generator is
narrow, and the number alone does not say which. It was the second:
until recently the generator emitted no enums, no `switch`, no
compound assignment and no increment operators, so agreement about
those constructs was agreement about nothing.

`t/gen.py` writes a random C program. gcc and clang say what it should
return. The generator being ours costs nothing, because **the generator is
not the oracle**.

The difficulty is emitting C that is *defined*. A
random expression is overwhelmingly likely to overflow a signed int, divide
by zero, or shift by a negative amount, and at that point gcc and clang
are each entitled to a different answer, so a disagreement with us would
prove nothing. Arithmetic is therefore done in `unsigned long`, where
overflow is defined to wrap. Every divisor is `| 1` so it is odd and never
zero. Every shift count is `& 63`. That is also the honest limit of what it
can find. It will never catch a signed-overflow bug, because it never
generates one.

If gcc and clang disagree with **each other**, the generator let undefined
behaviour through. Those seeds are skipped and counted, because a silently
shrinking denominator is how a fuzzer stops meaning anything.

The census is the same idea one level up. A construct that stops being
generated shrinks what is being fuzzed without shrinking any number
the fuzzer prints, so the constructs are counted from the generated
sources and each has a floor. Disabling the enum branch does not
produce "60 agree"; it produces a refusal to report at all.

Tuning them was a measurement too. At the first attempt the four new
constructs appeared in about one program in thirty, and the fuzzer
reported 300 agree either way. The frequencies and the number of
top-level blocks were raised until the census said they were actually
being reached.

The last row of the table below is the one that justifies the work:
enum signedness is only observable through a bit-field of enum type,
it was a real miscompilation here a day ago, and the generator that
ran 400 clean seeds could not have found it, because it never wrote
the word `enum`.

And because a fuzzer that always passes is worth nothing, it was checked
against planted defects, not assumed to work:

| planted defect | seeds that caught it |
|---|---|
| `xor` emitted as `or` | 44 of 100 |
| unsigned division emitted as signed | 26 of 100 |
| a narrow parameter stored at full width | 25 of 100 |
| a struct passed in one register instead of two | 18 of 100 |
| a signed `char` loaded with `movzbl` | 11 of 100 |
| shift count taken from the wrong place | 13 of 100 |
| an argument not converted at the call | 5 of 100 |
| enum signedness not derived from the enumerators | 17 of 60 |

Three of those numbers started at zero, and fixing them is most of what
this generator is:

Generating a construct is not exercising it. The first version emitted
structs and narrow types, then threw most of their values away, a wrong
answer was computed and never reached the exit status. Every construct now
folds into a running accumulator, which took "struct passed in one
register" from 1 of 100 to 18.

A signed type needs negative values. Narrow variables were masked to
0..127, so the sign bit was never set and a signed load was
indistinguishable from an unsigned one: 0 of 100. They now land in -64..63,
which still keeps `a*b` inside an int.

Narrowing cannot test a conversion at the call. Passing a wide value to
a narrow parameter is right either way, because the callee truncates into
its own slot: the same vacuity as `g(300)` further down this file. Only
widening can see it, so every narrow variable is also passed to a function
taking `long`.

### Checking a type, not just a number

The usual arithmetic conversions are verified by 368 generated expressions,
every operand type against every integer operator, plus a separate sweep
for shifts, which are not symmetric and whose count has to stay in range.

Each is compared with gcc on three things at once. The sign of the
result, whether it is zero, and its `sizeof`. That last one is the point. A
wrong type very often computes the right number, and this item existed
because of exactly that case: `u - 1` on an unsigned `u` produced precisely
the right bits and then described itself as an `int`, so the comparison
after it went signed. A suite that only checks values cannot see it.

### The bug that found itself

Writing those cases turned up a miscompilation nothing was looking for:

```c
unsigned x = -1;
x < 5     // 0, correct
x > 5     // 0, and it should be 1
```

`>` is parsed as `<` with the operands swapped, and the signedness of the
comparison was read from the left operand alone. After the swap that is
whichever side the literal landed on, so the same variable compared unsigned
one way round and signed the other. It now follows C 6.3.1.8 and looks at
both, which also fixes `2 / x` for unsigned `x` on the right.

Two things that rule must *not* do, each with its own case in the suite
because `>` and `<` put the operands in opposite positions:

- an `unsigned char` promotes to signed `int`, so `x > -1` is true
- an `unsigned int` widens to `long` on LP64, so `l < u` stays signed

The first mutant written for this was itself wrong, and worth recording. It
removed the promotion from the *left* operand while the test case had the
narrow type on the *right*, so identical assembly came out and the mutant
looked like it survived. A mutation test can be vacuous exactly like an
ordinary one.

### A mutant the plain suite cannot kill

Global initialisers were mutation tested the same way, and one survivor is
the reason `make test-asan` now exists. Removing the clamp here

```c
int n = t->str_len < ty->size ? t->str_len : ty->size;   // char s[3] = "hello"
```

turns `char s[3] = "hello"` into a six-byte write into a three-byte buffer.
Re-measured with that clamp removed: the ordinary suite prints `408 passed,
0 failed` and the sanitizer build prints `407 passed, 1 failed`, naming the
case. Bytes the mutant tramples are not ones the output depends on, so the
program still returns 57 and every ordinary assertion holds. A test that drives the code over the bug and checks the
answer is not the same as a test that checks the bug did not happen.

### Testing an ABI you are only half of

Variadic functions were the first feature where the interesting cases could
not be written in the language being compiled.

The real oracle is glibc. `vsprintf(buf, fmt, ap)` hands our `va_list`
to a library that was compiled years ago by a different compiler and walks
it with its own `va_arg`. If the struct layout, the starting `gp_offset`, or
the register save area are wrong by one field, glibc reads someone else's
memory instead of our arguments. No assertion we could write about our own
data structures comes close.

The second case runs the other way. `va_arg`'s overflow branch: arguments
seven and beyond, which arrive on the stack, not in registers, is
unreachable from any program this compiler builds, because its own call path
stops at six arguments. So the suite has gcc make the call. It compiles a
`main` that passes nine arguments to a variadic function we compiled,
and links them together. Their calling convention, our prologue, our
`va_arg`. Delete the `jae` that chooses the overflow path and only that one
case fails.

Two mutants are left alive deliberately. Replacing `calloc` with `malloc` in
the initialiser buffer should break partial initialisation: `int a[4] =
{1,2}` must zero the rest, and does not, because this compiler never frees
anything, so every allocation is a fresh page the kernel has already zeroed.
The line is still correct and still required the moment any of that changes.
Nothing in this suite can distinguish it, and saying so is better than
pretending the coverage is complete.

The second is `va_list`: it is an array of one struct, per the ABI, and
making it a plain struct changes nothing here. `is_address_valued()` is true
for every aggregate in this compiler, so a struct yields its address exactly
as an array does. The array is still correct and still what C specifies; it
simply stops mattering until aggregates can be passed by value. Its first version claimed the struct form would silently pass a copy,
which was a confident explanation of something that does not happen.

## 220 programs nobody here wrote

```
$ make conformance
c-testsuite: 195 of 220 pass (88%)
  no regressions against t/cts-baseline.txt
  by kind:
       25 refused
```

Every other oracle here compares cc against gcc and clang on programs
this repository wrote. That answers "do the three of us agree about my
test cases". This corpus answers a different question, which is how much
of the language is actually here, and the only way to ask it is with
cases nobody here chose.

The two kinds of failure are never merged into one number. A refusal is
a gap: cc declined, which is the honest answer to a feature it does not
have. A miscompilation is a bug: cc accepted the program and produced a
binary that crashed or printed the wrong thing.

All twenty-five remaining failures are the first kind. There were four
of the second, all of them invisible until this corpus ran, and all
four are fixed:

- **00078**, a function declared at block scope. It fell through to
  the local-variable path, got a stack slot, and the call went through
  those eight bytes. Six lines, below.
- **00212**, `__LP64__` not predefined. The one-line fix would have
  been the wrong size: gcc predefines 400 macros here and cc had 44,
  so the fix was the 23 platform macros a real program tests, with gcc
  computing every expected value.
- **00218**, enum signedness. `new_enum_type` never set `is_unsigned`,
  so an eight-bit bit-field of enum type read 150 back as -106. C11
  leaves the compatible type implementation-defined and the rule came
  from gcc: unsigned when no enumerator is negative.
- **00206**, `#pragma push_macro` and `pop_macro`. cc skipped every
  pragma, which is right for the ones that change nothing observable
  and wrong for these two: the program kept building with the wrong
  macro body and printed the wrong string twice, exiting 0.

That last one is the clearest case of the distinction this whole
section is about. Skipping an unknown pragma is what the standard
asks for. Skipping a known one that changes what the preprocessor
produces is a miscompilation wearing a shrug.

A fifth belongs on that list and did not reach it, because the runner
reported it as a refusal. **00095**, **00096** and **00136** failed
with three lines from the assembler:

```
00095.c.cc.s:8: Error: symbol `x' is already defined
```

cc had accepted the program, emitted a label for every declaration of
the name, and handed gas code it could not assemble. gas exited
non-zero, so cc exited non-zero, so `t/cts.sh` scored a refusal. cc
had diagnosed nothing. C11 6.9.2p2: `int x;` at file scope is a
tentative definition and however many of them a translation unit holds,
they all name one object. The runner still cannot tell this apart from
a real refusal, and that is the honest statement of its limit: it sees
an exit status, and a compiler that declines and a compiler that emits
broken assembly both produce one.

The smallest of them is six lines:

```c
int f1(char *p) { return *p + 1; }
int main(void) {
	char s = 1;
	int f1(char *);          /* a declaration at block scope */
	return f1(&s) == 2 ? 0 : 1;
}
```

gcc compiled that to something that exits 0 and cc compiled it to
something that segfaulted. A function declared inside a block declares a
function with external linkage and no storage, C11 6.2.2p5, and it was
falling through to the local-variable path, taking a stack slot, so the
call went through whatever those eight bytes held. Fixed, with four
cases added to `t/run.sh`: the corpus is fetched and skips offline, and
a fix with no local test is a fix waiting to be undone.

### The twenty-five that are left

Grouped by cause rather than listed, because sixteen causes is the
useful number and twenty-five case numbers is not:

- floating-point arithmetic, 7 cases (00113, 00119, 00123, 00174,
  00175, 00195, 00204). The largest single group and the largest piece
  of work.
- anonymous `struct` and `union` members, 2 (00046, 00050)
- GNU statement expressions, `({ ... })` as an expression, 2 (00213, 00214)
- a forward-declared `enum` tag, which is not ISO C and which gcc
  accepts, 2 (00170, 00209)
- brace elision in a nested initialiser, 1 (00205)
- a declaration mixing function and object declarators, `int f(int),
  g(int), a;`, 1 (00121)
- a function returning a function pointer, 1 (00124)
- an empty struct, plus a compound literal in a static initialiser, 1 (00216)
- `_Generic`, 1 (00219)
- `__attribute__((packed))`, 1 (00210)
- a variable-length array, 1 (00207)
- a qualifier inside an array declarator, `int x[const 5]`, 1 (00162)
- a call through a cast to a function-pointer type, 1 (00159)
- wide string literals, 1 (00220)
- a 32-byte struct parameter, the MEMORY class, 1 (00140)
- the `#line` directive, 1 (00152)

The gate is no regression rather than a pass rate, since cc implements a
subset on purpose and most of these failures are expected. The baseline
is checked in both directions: a case that starts passing has to be
removed from it, or the file rots into a list nobody rechecks. Both
directions were tested by breaking them.

The corpus is fetched rather than vendored, pinned to one commit and
checked by sha256. Its per-test metadata records where each case came
from and gives no licence, so redistributing them here would be a claim
about rights nobody here holds. Offline, the target skips and says so.

## Real headers

`<stdio.h>`, `<string.h>`, `<ctype.h>`, `<stdint.h>`, `<limits.h>`,
`<stdbool.h>`, `<stddef.h>`, `<stdarg.h>`, `<stdlib.h>`, `<wchar.h>`,
`<math.h>` and `<float.h>` are compiled from `/usr/include`, and a program
calling `printf`, `sprintf` and `strcmp` through them runs. That is every
header this compiler has been pointed at.

`<wchar.h>` arrived with `L'x'` character constants and nothing else;
`<math.h>` and `<float.h>` arrived with the decision to refuse a floating
constant where it is used rather than where it is lexed. Neither needed
any arithmetic: a header is declarations, and this compiler can read a
declaration of a type it will not operate on.

`<limits.h>` needed `#include_next`, which exists so that gcc's own
`limits.h` can reach glibc's: two files of the same name in different
directories, each deliberately pulling in the other. Searching from the
start of the path list finds the same file again and recurses, so a file
records which search path found it and the next search resumes after that.

`<stdint.h>` needed the `__INT_LEAST8_TYPE__` family. gcc's own header asks
the compiler what its integer types are called instead of hardcoding them,
so a compiler that answers nothing gets nowhere.

Not defining `__GNUC__` turns out to help, not hurt: glibc hands a
compiler that does not claim to be gcc the portable declarations, with no
`__attribute__` on them. That also means a preprocessed system header
cannot be diffed against `gcc -E`: the two compilers are not reading
the same source. The differential oracle this project leans on stops meaning
anything the moment the input depends on which compiler is asking, so for
headers the suite asks the weaker question in `t/cpp.sh` (does it come out
whole) and the stronger one in `t/run.sh` (does it compile and run).

## License

GPL-3.0-or-later. This is a tool, not a library. I would rather a fork
of it stay open than get embedded in something closed.
