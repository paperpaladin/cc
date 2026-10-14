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
ok   cc2 passes t/run.sh  (325 passed, 0 failed)
ok   cc2 passes t/cpp.sh  (64 passed, 0 failed)
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
325 passed, 0 failed
64 passed, 0 failed

$ make diff-test   # the oracle that matters
282 agree with gcc+clang, 0 disagree, 7 skipped
10 multi-line case(s) in t/run.sh are not differential-tested
```

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
`__LINE__`, `#error`, and `-D`/`-U` on the command line. `-E` prints the
result.

### What does not

**The preprocessor does not have `_Pragma`, `#include_next`, `__DATE__`,
`__TIME__` or `__COUNTER__`**, and it does not police the constraint that a
redefinition be identical: a second `#define` silently replaces the first.
A stray backslash outside a string literal is rejected, not carried
along as a preprocessing token, which is legal C that real preprocessors
accept. Nothing here needs it, and the tokenizer is simpler without it.

No floating-point arithmetic. `float`, `double` and `long double` are
declarable: they parse, size correctly at 4, 8 and 16, and appear in
declarations, enough for real headers, but producing a value of
one is an error, not a silent miscompilation. No `va_arg` of a
struct.

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

One file. Splitting a 4,764-line compiler into `lex.c`/`parse.c`/`codegen.c`
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
  figure cannot be re-run from this repo and `cc.c` is 4,764 lines now.
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

`t/addcase.sh` adds a case with its expected value **taken from gcc**:

```
$ ./t/addcase.sh 'int main() { int a[]={1,2,3}; return sizeof(a); }'
added: expected 12 (gcc and clang agree)
```

Five of my hand-computed expectations have been wrong across this project, each
one costing a detour through "is this a codegen bug?" The compiler was right every
time. So I stopped guessing, gcc already knows, and if gcc and clang disagree the
program relies on undefined behaviour and has no business in the suite.

`t/cpp.sh` is the same idea aimed at the preprocessor: 64 cases, every
expectation produced by **gcc -E -P and clang -E -P**, both of which must
agree with us. It includes C99 6.10.3.5 examples 3 and 5 verbatim, which are
the published torture tests for rescanning, `#`, `##` and variadic macros.

```
$ make test
325 passed, 0 failed
64 passed, 0 failed

$ make test-asan        # the same suite, watched
325 passed, 0 failed
64 passed, 0 failed
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
fails (measured, "0 passed, 325 failed") since every run ends holding its
whole AST. All of them saying the same known thing is the fastest way to stop
reading output.

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
$ make fuzz FROM=2000 TO=2600
601 agree, 0 disagree, 0 skipped (seeds 2000..2600)
```

About 1,360 distinct seeds have been run across several ranges, with no
disagreement.

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
The ordinary suite scores it 237 passed. Bytes it tramples are not ones
the output depends on, so the program still returns 57 and every assertion
holds. Under the sanitizer build the same case is a heap-buffer-overflow and
the mutant dies. A test that drives the code over the bug and checks the
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

## Real headers

`<stdio.h>`, `<string.h>`, `<ctype.h>`, `<stdint.h>`, `<limits.h>`,
`<stdbool.h>`, `<stddef.h>`, `<stdarg.h>` and `<stdlib.h>` are compiled from
`/usr/include`, and a program calling `printf`, `sprintf` and `strcmp`
through them runs. That is every header this compiler has been pointed at.

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
