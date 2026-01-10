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
is organized around that: the real test suite doesn't contain a single
hand-written expected value.

That turned out to matter on the first run. Two cases failed, I assumed two
codegen bugs, and both were wrong expectations I had computed by hand
(`-17 % 5` is `-2`, not `-3`). The differential harness would have caught them
without the detour.

## Status

```
$ make test        # exit-status assertions
168 passed, 0 failed

$ make diff-test   # the oracle that matters
166 agree with gcc+clang, 0 disagree, 2 skipped
```

The 2 skips are `printf`/`strlen` without a declaration -- clang 19 defaults to
C23, where implicit function declarations are an error, so it won't build them
as a reference.

### What works

**Types** -- `void`, `_Bool`, `char`, `short`, `int`, `long`, their `unsigned`
forms, pointers, arrays (multidimensional, with the `a[i] == *(a+i)` identity),
`struct`, `union`, `enum`, and `typedef`. Struct layout computes real alignment
and padding; unions overlay at offset 0. Incomplete types are tracked, so a
forward-declared `struct` can be referenced through a pointer and completed later.

**Expressions** -- full operator precedence, short-circuit `&&`/`||`, ternary,
comma, compound assignment, pre/post increment and decrement, `sizeof` over both
expressions and type names, casts with correct signed/unsigned width conversion,
member access through `.` and `->`, struct assignment, and constant folding for
array dimensions and enum values.

**Statements** -- `if`/`else`, `for`, `while`, `do`, `break`, `continue`,
`switch`/`case`/`default` with fallthrough, `goto` and labels, and real block
scoping so an inner declaration shadows an outer one.

**Initialisers** -- `int a[3] = {1,2,3}`, `int a[] = {1,2}` with inferred length,
`char s[] = "abc"` (4 bytes, the NUL is part of the object), `struct P p = {4,5}`,
and nested forms: arrays of structs, structs containing arrays, 2D arrays.
Omitted elements are zero-initialised explicitly, because a local lives on an
uninitialised stack.

**Calls** -- definitions and calls up to 6 arguments, recursion, and calls into
libc that get the ABI right.

**Bitfields** -- packing, `:0` to break a storage unit, unnamed padding
fields, signed and unsigned, up to the full width of the underlying type.
Layout matches gcc on every case in the suite. A field that will not fit
whole in the open unit starts a new one rather than straddling.

**Variadic definitions** -- `int sum(int n, ...)` with the SysV register
save area, plus `va_list`, `va_start`, `va_arg`, `va_end` and `va_copy`
predefined, so none of it needs a header. The real `<stdarg.h>` is GNU C
from its first line, which is exactly the dependency this avoids.

**Global initialisers** -- scalars, arrays, nested arrays, structs, strings
into `char` arrays, and addresses: `char *p = "abc"`, `int *p = &v`, `int *p
= a + 2`, and tables like `char *K[] = {"a","bb",0}`. An address is not a
byte pattern, so those become relocations the linker fills in. Initialised
`static` locals work too. Uninitialised globals go in `.bss`, initialised
ones in `.data`, string literals in `.rodata`.

**`static` and `extern`** -- internal linkage on globals and functions,
`extern` declarations that reference another translation unit without
defining anything, and `static` locals, which have block scope and static
storage and so are emitted as globals under a generated name.

**Function prototypes** -- a declaration gives a call its return type, so
`sizeof(f())` is right and a returned pointer can be dereferenced. Arguments
are converted to their parameter types at the call, and the argument count is
checked. `f(void)` and `f()` are kept apart: the first promises no arguments
and is checked, the second promises nothing and is not. A call to an
undeclared function still works and still assumes it returns `long`.

**Preprocessor** -- `#include` (both spellings, with `-I`), object and
function-like macros, `#undef`, the full `#if`/`#ifdef`/`#ifndef`/`#elif`/
`#else`/`#endif` family with constant-expression evaluation and `defined`,
`#` stringification, `##` pasting, variadic macros including the GNU
`, ## __VA_ARGS__` comma deletion and the GNU `args...` naming, `__FILE__`,
`__LINE__`, `#error`, and `-D`/`-U` on the command line. `-E` prints the
result.

### What does not

**The preprocessor does not have `_Pragma`, `#include_next`, `__DATE__`,
`__TIME__` or `__COUNTER__`**, and it does not police the constraint that a
redefinition be identical -- a second `#define` silently replaces the first.
A stray backslash outside a string literal is rejected rather than carried
along as a preprocessing token, which is legal C that real preprocessors
accept; nothing here needs it, and the tokenizer is simpler without it.

No `float` or `double`. No `va_arg` of a struct. `const`, `volatile` and
`restrict` parse and are then ignored -- there is no const-checking here,
and saying so is better than implying there is.
A call may pass at most six arguments (a definition may *receive* more than
six through `va_arg`, which is how the overflow path gets tested at all). No variadic function
*definitions* (calling a variadic function works). Passing or returning a struct
**by value** is rejected with an explicit error rather than silently
miscompiled -- that needs the SysV eightbyte classification algorithm, which is
its own milestone.

It cannot compile itself yet. The preprocessor was the largest blocker and is
now in; what remains is function prototypes, `static`/`extern`, global
initialisers and the rest of milestone 1. The unsupported list is ordered by
what `cc.c` itself uses, so that self-hosting arrives as early as possible --
see `PLAN.md`.

## Design

One file. Splitting a 1,200-line compiler into `lex.c`/`parse.c`/`codegen.c`
buys nothing while it still fits in a single screen of `ctags`, and a single
translation unit is easier to self-host later.

Three deliberate corners, each marked `shortcut:` in the source with its
ceiling:

- **Stack-machine codegen, no register allocator.** Every binary operator
  pushes and pops. The output is roughly 3x the instructions a real compiler
  emits. Worth fixing only once self-hosting is green and compile time matters.
- **Nothing is freed.** A compiler is a short-lived process; `exit` reclaims.
- **`gcc` assembles and links.** Writing an assembler is a different project.

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
indirect self-reference fall out of the same rule rather than needing their
own special case. With the hide-set test removed, `f(1)` does not terminate --
see the mutation table below.

### `-E` output has to read back

Expansion puts tokens next to each other that were never adjacent in the
source, and some of those pairs lex differently when printed touching. `2` and
`+` are the dangerous case: a pp-number may contain an exponent, so `2+` is one
token to a conforming lexer. The printer therefore inserts a space wherever two
adjacent tokens could glue, and the suite asserts the resulting guarantee
directly -- running `-E` over its own output must produce that output again.

Worth noting as a calibration: C99 6.10.3.5's printed example shows `2+(3,4)`,
and gcc, clang and this compiler all emit `2 +(3,4)`. The implementations are
right and the standard's illustrative text is loose.

## Tests

`t/run.sh` holds the cases and asserts exit statuses. `t/differential.sh`
re-runs the same cases but throws the expectations away and compares against gcc
and clang instead -- if the two reference compilers disagree with each other the
case is skipped as undefined behavior rather than counted against us.

Cases live in exactly one place. `differential.sh` greps them out of `run.sh`
rather than keeping a second copy in sync.

`t/addcase.sh` adds a case with its expected value **taken from gcc**:

```
$ ./t/addcase.sh 'int main() { int a[]={1,2,3}; return sizeof(a); }'
added: expected 12 (gcc and clang agree)
```

Five of my hand-computed expectations have been wrong across this project, each
one costing a detour through "is this a codegen bug?" The compiler was right every
time. So I stopped guessing -- gcc already knows, and if gcc and clang disagree the
program relies on undefined behaviour and has no business in the suite.

`t/cpp.sh` is the same idea aimed at the preprocessor: 56 cases, every
expectation produced by **gcc -E -P and clang -E -P**, both of which must
agree with us. It includes C99 6.10.3.5 examples 3 and 5 verbatim, which are
the published torture tests for rescanning, `#`, `##` and variadic macros.

```
$ make test
237 passed, 0 failed
56 passed, 0 failed

$ make test-asan        # the same suite, watched
237 passed, 0 failed
56 passed, 0 failed
```

`make test-asan` rebuilds the compiler with AddressSanitizer and
UndefinedBehaviorSanitizer and runs everything again. Two settings in that
target are load-bearing rather than tidy:

`ASAN_OPTIONS=exitcode=86`, because a sanitizer abort exits 1 and so does a
correct refusal. Without separating them, `refuse()` scores a crash as a
pass and the entire sanitizer build quietly stops meaning anything. That
exact collision is a bug already recorded in a sibling project, so it was
cheaper to assume it than discover it.

`detect_leaks=0`, because this compiler frees nothing on purpose and
LeakSanitizer is right every single time it complains. 237 failures that all
say the same known thing is the fastest way to stop reading output.

### Watching that suite fail

A green suite means nothing until it has been seen to go red, so seven defects
were planted in the preprocessor one at a time:

| planted defect | result |
|---|---|
| hide set never consulted | caught -- `f(1)` does not terminate |
| `#` does not escape quotes or backslashes | caught, 1 case |
| the operand of `##` is expanded first | caught, 2 cases |
| no space inserted at a paste hazard | caught by the idempotence check |
| an expansion keeps its definition site | caught -- `__LINE__` reports the `#define` |
| an argument is substituted unexpanded | caught, 2 cases |
| `#elif` is evaluated after a branch was taken | caught, 1 case |

All seven. The fourth is the one worth having: it fails nothing about what the
tokens *are*, only that printing them loses information, and it is caught only
because the suite re-reads its own output rather than trusting it.

The same was done for prototypes, and there the exercise paid for itself
immediately. Seven defects, six caught, and the survivor was **"arguments are
not converted at the call"** -- which the suite was supposed to cover.

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

The first mutant written for this was itself wrong, and worth recording: it
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
The ordinary suite scores it 237 passed: the bytes it tramples are not ones
the output depends on, so the program still returns 57 and every assertion
holds. Under the sanitizer build the same case is a heap-buffer-overflow and
the mutant dies. A test that drives the code over the bug and checks the
answer is not the same as a test that checks the bug did not happen.

### Testing an ABI you are only half of

Variadic functions were the first feature where the interesting cases could
not be written in the language being compiled.

The real oracle is **glibc**. `vsprintf(buf, fmt, ap)` hands our `va_list`
to a library that was compiled years ago by a different compiler and walks
it with its own `va_arg`. If the struct layout, the starting `gp_offset`, or
the register save area are wrong by one field, glibc reads someone else's
memory instead of our arguments. No assertion we could write about our own
data structures comes close.

The second case runs the other way. `va_arg`'s overflow branch -- arguments
seven and beyond, which arrive on the stack rather than in registers -- is
unreachable from any program this compiler builds, because its own call path
stops at six arguments. So the suite has gcc make the call: gcc compiles a
`main` that passes nine arguments to a variadic function **we** compiled,
and links them together. Their calling convention, our prologue, our
`va_arg`. Delete the `jae` that chooses the overflow path and only that one
case fails.

Two mutants are left alive deliberately. Replacing `calloc` with `malloc` in
the initialiser buffer should break partial initialisation -- `int a[4] =
{1,2}` must zero the rest -- and does not, because this compiler never frees
anything, so every allocation is a fresh page the kernel has already zeroed.
The line is still correct and still required the moment any of that changes.
Nothing in this suite can distinguish it, and saying so is better than
pretending the coverage is complete.

The second is `va_list`: it is an array of one struct, per the ABI, and
making it a plain struct changes nothing here. `is_address_valued()` is true
for every aggregate in this compiler, so a struct yields its address exactly
as an array does. The array is still correct and still what C specifies; it
simply stops mattering until aggregates can be passed by value. The first
version of this comment claimed the struct form would silently pass a copy,
which was a confident explanation of something that does not happen.

## Real headers

`<stddef.h>`, `<stdarg.h>`, `<string.h>`, `<ctype.h>`, `<stdint.h>`,
`<limits.h>` and `<stdbool.h>` are compiled from `/usr/include` and the
resulting programs run. Two things stop the rest:

| header | stops at |
|---|---|
| `<stdio.h>` | a function typedef |
| `<stdlib.h>` | `typedef float _Float32` |

`<limits.h>` needed `#include_next`, which exists so that gcc's own
`limits.h` can reach glibc's: two files of the same name in different
directories, each deliberately pulling in the other. Searching from the
start of the path list finds the same file again and recurses, so a file
records which search path found it and the next search resumes after that.

`<stdint.h>` needed the `__INT_LEAST8_TYPE__` family. gcc's own header asks
the compiler what its integer types are called rather than hardcoding them,
so a compiler that answers nothing gets nowhere.

Not defining `__GNUC__` turns out to help rather than hurt: glibc hands a
compiler that does not claim to be gcc the portable declarations, with no
`__attribute__` on them. That also means a preprocessed system header
**cannot** be diffed against `gcc -E` -- the two compilers are not reading
the same source. The differential oracle this project leans on stops meaning
anything the moment the input depends on which compiler is asking, so for
headers the suite asks the weaker question in `t/cpp.sh` (does it come out
whole) and the stronger one in `t/run.sh` (does it compile and run).

## License

GPL-3.0-or-later. This is a tool rather than a library -- I would rather a fork
of it stay open than get embedded in something closed.
