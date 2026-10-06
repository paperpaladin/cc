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

104 hand-written cases pass. 102 of them are also verified against **both** gcc
and clang, with zero disagreements.

```
$ make test        # exit-status assertions
104 passed, 0 failed

$ make diff-test   # the oracle that matters
102 agree with gcc+clang, 0 disagree, 2 skipped
```

The 2 skips are `printf`/`strlen` without a declaration -- clang 19 defaults to
C23, where implicit function declarations are an error, so it won't build them
as a reference.

### What works

Integers, `char`, `short`, `long`, pointers, arrays (including multidimensional
and the `a[i] == *(a+i)` identity), globals, string literals with escapes,
`sizeof`, full operator precedence with correct short-circuit `&&`/`||`,
ternary, comma, compound assignment, pre/post increment and decrement,
`if`/`else`/`for`/`while`/`do`/`break`/`continue`, function definitions and
calls up to 6 arguments, recursion, and calls into libc.

### What does not

No preprocessor, so no `#include` and no macros. No `struct`, `union`, `enum`,
`typedef`, `float`, `double`, `unsigned`, `switch`, `goto`, or casts. Only one
scope per function, so an inner block cannot shadow an outer variable. Function
calls assume a `long` return type because there are no prototypes yet.

It cannot compile itself. That is the next milestone and the reason the
unsupported list above is ordered the way it is -- see `PLAN.md`.

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

## License

GPL-3.0-or-later. This is a tool rather than a library -- I would rather a fork
of it stay open than get embedded in something closed.
