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
- [x] 02 scopes, typedef, struct, union, enum, unsigned
      why: one interlocking change; declspec cannot gain typedef without also
           learning struct/union/enum and the unsigned widths
      proves: struct layout with real padding, union overlay, shadowing,
              self-referential structs, struct assignment
- [x] 03 cast expressions and gen_cast width conversion
      why: could not land before typedef, since "(x)" is a cast only if x names
           a type in scope
      proves: 15 cases covering truncation and both extension directions
- [x] 04 switch/case/default, goto and labelled statements
      proves: fallthrough, break binding to the switch rather than the loop,
              goto out of a loop, forward and backward jumps
- [x] 05 initialisers for locals, arrays, structs and strings
      why: lowered to element assignments, so nested forms work without new
           codegen
      proves: inferred lengths, zero fill, 2D arrays, arrays of structs
- [x] 06 t/addcase.sh -- expectations derived from gcc, cross-checked with clang
### Milestone 1 -- self-host

Everything remaining is ordered by what `cc.c` itself uses. Nothing here is
speculative; each is a feature the compiler's own source needs.

The preprocessor (07) is done. What `cc.c` still needs from the list below,
by inspection of its own source: function prototypes, `static`, global
initialisers, and variadic definitions for the error helpers.

- [x] 07 the preprocessor: `#include`, object and function macros, `#if`/`#ifdef`,
      token pasting, stringification, and the recursive-expansion hide-set rules
      why: no `#include <stdio.h>` means no self-host. **The biggest single chunk
           left -- expect it to take longer than 02 through 06 put together.**
      note: glibc's headers weaponise GNU C. `__attribute__`, statement
            expressions, `typeof`, `__builtin_*` and asm labels all appear before
            you reach anything useful. This is the main schedule risk in the
            whole project.
      done: in two commits -- the tokenizer gaining files, lines and at_bol
            first, with no behaviour change, then the preprocessor itself.
            56 cases in t/cpp.sh, every expectation from gcc -E -P and
            clang -E -P, both of which must agree. C99 6.10.3.5 examples 3
            and 5 are in verbatim.
            Three bugs the oracle found, in rising order of interest:
            skip_line() returned its argument unchanged when that argument
            already began a line, so skipping a false #if never advanced and
            hung; string concatenation was being done inside preprocess(),
            which is phase 6 run during phase 4 and made `S(a) S(b)` print
            one literal; and an expansion kept the line of its #define, so
            __LINE__ inside a macro body reported where the macro was written
            rather than where it was used.
            The GNU note above has not been tested yet. Nothing here has met
            /usr/include, and item 15 is where that bill arrives.
      proves: 7 planted defects, 7 caught -- see the table in the README.
- [x] 08 function prototypes, so a call stops assuming a `long` return
      done: the declaration side already parsed; the call site was the gap.
            A function now goes into the file scope (it was only ever added
            to the globals list, so find_name could not see it), a call takes
            its type from the declaration, arguments are converted as if by
            assignment, and the count is checked. A prototype followed by a
            definition is one Obj, not two.
            `f(void)` and `f()` are deliberately not the same: both produce an
            empty parameter list, so has_proto records which one was written
            or the difference is lost and `f()` starts rejecting its own
            callers. Unnamed parameters had to be allowed -- `int f(int);` is
            what every header writes -- and that immediately crashed, because
            an unnamed Obj was still being pushed into the scope and the first
            lookup that scanned past it called strlen(NULL).
            An undeclared call still assumes `long`. C89's implicit int would
            truncate a pointer from an undeclared malloc, and the suite calls
            printf and strlen with no declaration.
      proves: 7 planted defects. Six caught; the survivor is the interesting
            one and is written up in the README. "Arguments are converted at
            the call" was being tested by g(300) against int g(char), which
            returns 44 either way because the callee truncates into its own
            one-byte slot. Narrowing cannot see a call-site conversion. The
            widening case can, and kills the mutant.
      note: finding that turned up an unrelated miscompilation -- unsigned
            relational comparison uses the signed condition codes, so
            `unsigned x = -1; x > 0` is 0. Item 08b below.
- [x] 08b unsigned relational operators use the unsigned condition codes
      why: `unsigned x = -1; x > 0` returned 0 and should return 1. Found
           while building the prototype cases, not by anything looking for it.
      done: the codegen was already right -- it picks setb or setl from a
            `uns` flag. The flag was wrong: it read `n->lhs->ty` only, and
            since `>` is parsed as `<` with the operands swapped, that is
            whichever side the literal landed on. The same variable compared
            unsigned as `x < 5` and signed as `x > 5`.
            op_unsigned() now applies C 6.3.1.8 to both operands: promote
            anything narrower than int to signed int, the wider rank wins
            outright, and at equal rank unsigned wins. That also fixes
            `2 / x` for an unsigned x on the right, which had the same cause.
      note: the first mutant for this was vacuous. It removed the promotion
            from the left operand while the test case had the narrow type on
            the right, produced byte-identical assembly, and looked like a
            survivor. Both operand positions now have their own case.
- [x] 09 `static` and `extern` linkage, and `static` locals
      done: a storage class on the Obj, honoured at emission -- no .globl for
            a static, and nothing emitted at all for an extern, which would
            otherwise be a second definition of a symbol another object owns.
            A static local is the interesting one: block scope, static
            storage, so it is a global announced inside a function. Two
            functions may each have a static "n", so the label is mangled
            n.0, n.1 the way gcc does it -- the dot is the point, since no C
            identifier contains one and nothing in the source can collide.
            The source name is pushed into the block scope separately, or the
            variable is unreachable by the name it was written with.
            An initialiser on a static local is refused with a message naming
            item 10, rather than silently ignored.
      proves: 5 planted defects, 5 caught -- after the suite grew the one
            case that could see linkage at all. See below.
      note: "static global still gets .globl" survived the first mutation
            round, and correctly: a single-file program behaves identically
            whether or not its statics are exported. Internal linkage has no
            observable consequence until a second translation unit defines
            the same name. t/run.sh now builds three files, two of which
            each declare `static int g`, and links them. With the .globl
            restored the linker folds the two g's into one and the answer
            changes from 12 to 22 -- in practice it fails earlier, at
            "multiple definition of g".
- [ ] 10 global initialisers (`int x = 5;` at file scope, and string pointers)
      why: currently every global is `.zero`-filled
- [ ] 11 variadic function definitions, `stdarg.h` against the real ABI
- [ ] 12 bitfields
      why: `sqlite3.c` uses them, and they are the largest typical `c-testsuite`
           failure cluster
- [ ] 13 struct arguments and returns -- the SysV eightbyte classification
      why: the nastiest part of the ABI. Currently rejected with an explicit
           error rather than miscompiled, which is the right interim answer.
      proves: passing and returning structs of every size class across the gcc
              boundary
- [ ] 14 integer promotion done properly, rather than the current approximation
- [ ] 15 **self-host**: `cmp cc2 cc3` in CI
      proves: the fixpoint invariant, from here to the end of the project

### Milestone 2 -- external conformance

- [ ] 16 `c-testsuite` runner, publishing the pass rate with the failures named
- [ ] 17 csmith differential fuzzing against gcc and clang
      why: finds the cases I would never think to write. Every expectation I have
           written by hand in this project that was wrong, five so far, would have
           been caught here for free.
- [ ] 18 fix what 16 and 17 surface, one commit per root cause
- [ ] 19 compile `sqlite3.c`, then run sqlite's own test suite against the result

### Milestone 3 -- only after the above is green

- [ ] 20 replace the stack machine with a register allocator; keep the fixpoint green
- [ ] 21 emit ELF objects directly, dropping the `gcc` dependency (see ../ld)

## Open questions

- `long double`: planning to alias it to `double` and document the lie rather
  than implement x87 80-bit. That is a deliberate known-wrong answer, which is
  different from an unimplemented one, so it has to be stated in the README.
- Struct assignment currently emits an unrolled byte copy, so a 4KB struct would
  produce 8K instructions. Fine today; needs `rep movsb` or a loop before
  anything real is compiled.
- Compound assignment (`a += b`) evaluates its lvalue twice, which is wrong for a
  side-effecting lvalue like `*p++ += 1`. Needs a hidden temporary. Nothing in
  `t/` trips it, so it is marked `shortcut:` in the source rather than fixed.
- `switch` compiles to a linear chain of compares. A jump table would be better
  for a dense switch and worse for a sparse one; no case in `t/` is large enough
  for the difference to be measurable yet.
