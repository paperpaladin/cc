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

### What does not

**No preprocessor.** No `#include`, no macros, no conditional compilation. This
is the single largest remaining gap and the main blocker on self-hosting.

No `float` or `double`. No `static` or `extern` linkage. No function prototypes,
so a call assumes a `long` return. No bitfields. No variadic function
*definitions* (calling a variadic function works). Passing or returning a struct
**by value** is rejected with an explicit error rather than silently
miscompiled -- that needs the SysV eightbyte classification algorithm, which is
its own milestone.

It cannot compile itself yet. The unsupported list is ordered by what `cc.c`
itself uses, so that self-hosting arrives as early as possible -- see `PLAN.md`.

## Design

One file. Splitting a 1,200-line compiler into `lex.c`/`parse.c`/`codegen.c`
buys nothing while it still fits in a single screen of `ctags`, and a single
translation unit is easier to self-host later.

Three deliberate corners, each marked `shortcut:` in the source with its
ceiling:

- **Stack-machine codegen, no register allocator.** Every binary operator
  pushes and pops. The output is roughly 3× the instructions a real compiler
  emits. Worth fixing only once self-hosting is green and compile time matters.
- **Nothing is freed.** A compiler is a short-lived process; `exit` reclaims.
- **`gcc` assembles and links.** Writing an assembler is a different project.

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

## License

GPL-3.0-or-later. This is a tool rather than a library -- I would rather a fork
of it stay open than get embedded in something closed.
