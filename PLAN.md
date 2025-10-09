# cc -- plan

## Contract

`cc [-S] [-o OUT] FILE.c`, a subset of the `cc(1)` interface. The real contract
is the **System V x86-64 ABI**: output must link against gcc-compiled objects
and call into glibc correctly. That is the line between a C compiler and a
C-shaped language.

## Hard invariant

**The fixpoint.** Once `cc` can compile itself:

```
./cc cc.c -o cc2 && ./cc2 cc.c -o cc3 && cmp cc2 cc3
```

`cc2` and `cc3` must be byte-identical. Any miscompilation that touches a code
path `cc` itself uses breaks this loudly. It is the cheapest regression test in
the project and costs one `cmp`.

Not yet reachable -- see milestone 1.

## Oracle

Ranked by how little I control them:

1. `t/differential.sh` -- gcc **and** clang must both agree with our exit status.
2. The fixpoint above.
3. `c-testsuite` pass rate, reported honestly including failures.
4. Compiling a real third-party program. `sqlite3.c` is the target.

Oracle 1 already caught two bugs -- in my own test expectations, which is the
point.

## Non-goals

Optimization. Any architecture but x86-64. Any OS but Linux. C++. DWARF debug
info. `long double` precision beyond `double`. Threads and `_Atomic`.

## Commit sequence

Each entry is one commit: the smallest change that moves the oracle, with the
test that proves it. Order is chosen so self-hosting arrives as early as
possible, because the fixpoint is worth more than any individual feature.

- [x] 00 tokenizer, parser, x86-64 codegen for the integer subset
      why: nothing can be tested until something runs end to end
      files: cc.c Makefile t/run.sh
      proves: `make test` -- 104 cases on exit status
- [x] 01 differential harness against gcc and clang
      why: hand-written expectations were wrong twice on the first run
      files: t/differential.sh
      proves: `make diff-test` -- 102 agree, 0 disagree

### Milestone 1 -- self-host

Ordered by what `cc.c` itself actually uses. Nothing here is speculative; each
is a feature the compiler's own source needs.

- [ ] 02 `struct` and `union`: member access, `.` and `->`, layout and padding
      why: `Token`, `Type`, `Node`, `Obj` are all structs. Hard blocker.
      proves: differential cases for nested structs and member assignment
- [ ] 03 `enum`
      why: `TokenKind`, `TypeKind`, `NodeKind`
- [ ] 04 `typedef`
      why: every `typedef struct Token Token;` in cc.c
- [ ] 05 block-local scope, so inner blocks shadow correctly
      why: currently one flat scope per function; cc.c relies on real scoping
      proves: a case where an inner `int i` must not clobber the outer one
- [ ] 06 the preprocessor: `#include`, object and function macros, `#if`/`#ifdef`,
      `#define`, token pasting, stringification
      why: no `#include <stdio.h>` means no self-host. Biggest single chunk.
      note: replaces the `cpp` shim; expect this to take longer than 02-05 put together
- [ ] 07 `switch` / `case` / `default`, `goto`, labels
- [ ] 08 casts and explicit conversions
- [ ] 09 `unsigned` types and the correct promotion rules
- [ ] 10 function prototypes, so calls stop assuming a `long` return
- [ ] 11 `static` and `extern` linkage
- [ ] 12 variadic functions (`stdarg.h` against the real ABI)
- [ ] 13 struct arguments and returns -- the SysV eightbyte classification
      why: the single nastiest part of the ABI, and where `sqlite3.c` will break
      proves: differential cases passing and returning structs of every size
              class across the gcc boundary
- [ ] 14 **self-host**: `cmp cc2 cc3` in CI
      proves: the fixpoint invariant, from here to the end of the project

### Milestone 2 -- external conformance

- [ ] 15 `c-testsuite` runner, publishing the pass rate with the failures named
- [ ] 16 csmith differential fuzzing against gcc and clang
      why: finds the cases I would never think to write
- [ ] 17 fix what 15 and 16 surface, one commit per root cause
- [ ] 18 compile `sqlite3.c`, then run sqlite's own test suite against the result

### Milestone 3 -- only after the above is green

- [ ] 19 replace the stack machine with a register allocator; keep the fixpoint green
- [ ] 20 emit ELF objects directly, dropping the `gcc` dependency (see ../ld)

## Open questions

- Bitfields: deferred. Typically the largest `c-testsuite` failure cluster, and
  `sqlite3.c` uses them. Will need to land before milestone 2 finishes.
- `long double`: planning to alias it to `double` and document the lie rather
  than implement x87 80-bit.
- glibc headers need a surprising amount of GNU C (`__attribute__`, statement
  expressions, `typeof`, `__builtin_*`, asm labels). Scope unknown until 06
  lands. This is the main schedule risk.
