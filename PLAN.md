# cc: plan

## Contract

`cc [-S] [-o OUT] FILE.c`, a subset of the `cc(1)` interface. The real contract
is the **System V x86-64 ABI**: output must link against gcc-compiled objects
and call into glibc correctly. That is the line between a C compiler and a
C-shaped language.

## Hard invariant

The fixpoint. `cc` compiles itself:

```
./cc cc.c -o cc2 && ./cc2 cc.c -o cc3 && cmp cc2 cc3
```

`cc2` and `cc3` are byte-identical. Any miscompilation touching a code path
`cc` itself uses breaks this loudly, and it costs one `cmp`.

Reached, and the qualification matters more than the milestone. The
fixpoint proves the compiler is STABLE, not correct. At the first green
`cmp cc2 cc3`, cc2 still failed 15 cases of t/run.sh, because a compiler
that miscompiles itself self-consistently reaches a fixpoint without
complaint. The invariant is therefore the fixpoint AND the stage-2 compiler
passing the whole suite, the thing `make self-host` asserts.

## Oracle

Ranked by how little I control them:

1. `t/differential.sh`, gcc and clang must both agree with our exit status.
2. The fixpoint above.
3. `c-testsuite` pass rate, reported honestly including failures.
4. Compiling a real third-party program. `sqlite3.c` is the target.

Oracle 1 already caught two bugs, in my own test expectations, and that is the
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
      proves: `make test`: 104 cases on exit status
- [x] 01 differential harness against gcc and clang
      why: hand-written expectations were wrong twice on the first run
      files: t/differential.sh
      proves: `make diff-test`: 102 agree, 0 disagree
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
      proves: fallthrough, break binding to the switch, not the loop,
              goto out of a loop, forward and backward jumps
- [x] 05 initialisers for locals, arrays, structs and strings
      why: lowered to element assignments, so nested forms work without new
           codegen
      proves: inferred lengths, zero fill, 2D arrays, arrays of structs
- [x] 06 t/addcase.sh, expectations derived from gcc, cross-checked with clang
- [x] 11b what self-hosting actually needs, found by trying it
      why: the remaining list was planned, not measured. Pointing cc at its
           own source and at /usr/include says which items are real.
      done: three gaps the plan did not have.
            Line splicing (phase 2) was simply missing. A backslash before a
            newline joins the lines before anything is a token, so it can cut
            an identifier in half; glibc uses it constantly.
            declspec parsed specifiers positionally, so "unsigned long" worked
            and "long unsigned int" did not, and stddef.h writes the second.
            Rewritten as a counter over the specifier multiset, which handles
            every ordering and makes "long long" fall out instead of being a
            case. Storage classes may now appear anywhere among them.
            Default system include paths, so <...> resolves without -I.
      state: <stddef.h> and <stdarg.h> compile and run. <string.h>, <stdio.h>,
            <stdlib.h>, <ctype.h> and <stdint.h> all stop at the same place,
            bitfields, item 12, and <limits.h> wants #include_next.
      note: a real header cannot be diffed against gcc -E, and the reason is
            structural: glibc branches on __GNUC__, so
            gcc and this compiler are handed different source. gcc gets the
            declarations with __attribute__ on them and we get the portable
            ones, and both are right. The differential oracle stops meaning
            anything the moment the input depends on which compiler is asking.
            t/cpp.sh therefore only asks whether the header comes out whole,
            and t/run.sh asks the stronger question by compiling and running
            what came out.
      bonus: not defining __GNUC__ turns out to be the useful choice rather
            than a shortfall. It is what makes glibc hand us declarations
            with no GNU extensions on them.

### Milestone 1: self-host

Everything remaining is ordered by what `cc.c` itself uses. Nothing here is
speculative. Each is a feature the compiler's own source needs.

Everything up to 12d is done, and the list below has been **re-derived from
`cc.c` itself**. Pointing the compiler at its own
source and at `/usr/include` is a cheaper way to learn what is missing than
reasoning about it, and it moved one item out of this milestone entirely and
brought two in that were never on the list.

What `cc.c` cannot compile about itself, in full: a function pointer as a
struct member and as a parameter, and one compound literal with designated
initialisers. That is the whole remaining distance to the fixpoint.

- [x] 07 the preprocessor: `#include`, object and function macros, `#if`/`#ifdef`,
      token pasting, stringification, and the recursive-expansion hide-set rules
      why: no `#include <stdio.h>` means no self-host. **The biggest single chunk
           left, expect it to take longer than 02 through 06 put together.**
      note: glibc's headers weaponise GNU C. `__attribute__`, statement
            expressions, `typeof`, `__builtin_*` and asm labels all appear before
            you reach anything useful. This is the main schedule risk in the
            whole project.
      done: in two commits: the tokenizer gaining files, lines and at_bol
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
            __LINE__ inside a macro body reported where the macro was
            written, not where it was used.
            The GNU note above has not been tested yet. Nothing here has met
            /usr/include, and item 15 is where that bill arrives.
      proves: 7 planted defects, 7 caught, see the table in the README.
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
            callers. Unnamed parameters had to be allowed, `int f(int);` is
            what every header writes, and that immediately crashed, because
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
      note: finding that turned up an unrelated miscompilation, unsigned
            relational comparison uses the signed condition codes, so
            `unsigned x = -1; x > 0` is 0. Item 08b below.
- [x] 08b unsigned relational operators use the unsigned condition codes
      why: `unsigned x = -1; x > 0` returned 0 and should return 1. Found
           while building the prototype cases, not by anything looking for it.
      done: the codegen was already right, it picks setb or setl from a
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
      done: a storage class on the Obj, honoured at emission: no .globl for
            a static, and nothing emitted at all for an extern, which would
            otherwise be a second definition of a symbol another object owns.
            A static local is the interesting one: block scope, static
            storage, so it is a global announced inside a function. Two
            functions may each have a static "n", so the label is mangled
            n.0, n.1 the way gcc does it, the dot is the point, since no C
            identifier contains one and nothing in the source can collide.
            The source name is pushed into the block scope separately, or the
            variable is unreachable by the name it was written with.
            An initialiser on a static local is refused with a message naming
            item 10, instead of silently ignored.
      proves: 5 planted defects, 5 caught: after the suite grew the one
            case that could see linkage at all. See below.
      note: "static global still gets .globl" survived the first mutation
            round, and correctly: a single-file program behaves identically
            whether or not its statics are exported. Internal linkage has no
            observable consequence until a second translation unit defines
            the same name. t/run.sh now builds three files, two of which
            each declare `static int g`, and links them. With the .globl
            restored the linker folds the two g's into one and the answer
            changes from 12 to 22, in practice it fails earlier, at
            "multiple definition of g".
- [x] 10 global initialisers (`int x = 5;` at file scope, and string pointers)
      why: currently every global is `.zero`-filled
      done: a byte buffer plus a sorted relocation list. An address cannot be
            a byte pattern, so "char *p = \"abc\"" is eight zero bytes and an
            instruction for the linker; eval_addr() is eval() extended to
            return label+addend for the forms a linker can resolve.
            Covers scalars, arrays, nested arrays, structs, strings into char
            arrays, &global, array decay, and pointer+offset, the whole of what
            cc.c's own KEYWORDS and PUNCT tables need.
            Sections split properly now: .bss for uninitialised, .data for
            initialised, .rodata for string literals.
            Unsized arrays had to learn their length before the incomplete
            type check, not after it, in both the global and the
            static-local path.
      found: "extern int e = 1;" was being refused. gcc accepts it and gcc is
            right, C11 6.9.2 says a file-scope declaration with an
            initialiser IS a definition. This is the first thing refuse()
            caught in cc, not in a test, the entire argument
            for making a refusal require an outside authority to agree.
      proves: 6 planted defects. 4 caught by `make test`, 1 more only by
            `make test-asan` (see below), 1 left alive on purpose.
- [x] 10b `make test-asan`
      why: ten sibling repos gate on sanitizers and this one did not, which
           is backwards: a compiler reads untrusted bytes for a living.
      done: prompted by a surviving mutant. Dropping the string-copy clamp
            makes `char s[3] = "hello"` a heap overflow inside the compiler,
            and the plain suite scores 237 passed, because the bytes it
            tramples are not ones any assertion reads. Under the sanitizer
            the same case fails. t/run.sh and t/cpp.sh take CCBIN so the
            cases are identical either way.
            ASAN_OPTIONS=exitcode=86 is load-bearing: a sanitizer abort exits
            1 and so does a correct refusal, so refuse() would have scored a
            crash as a pass. That is inflate'"'"'s recorded bug, borrowed rather
            than rediscovered.
            detect_leaks=0 is deliberate. Nothing here is ever freed, by
            design, so LeakSanitizer is correct on every run and 237 copies
            of a known complaint is how output stops being read.
      note: the surviving mutant is calloc -> malloc for the initialiser
            buffer. Partial initialisation must zero the rest, and the suite
            cannot tell, because nothing is freed and so every allocation is
            a page the kernel already zeroed. Correct, required, untestable
            from in here. Stated.
- [x] 11 variadic function definitions, `stdarg.h` against the real ABI
      done: the SysV register save area: all six GP registers spilled on
            entry to a known 48-byte block, because a variadic function has
            to hand out arguments that arrived in registers and registers
            are not addressable. Named parameters are therefore stored
            twice, as the ABI asks: the block must be a
            contiguous image of the registers for va_arg to index.
            va_list is built in the parser instead of reading from a header,
            since its layout is fixed by the ABI and the real <stdarg.h> is
            GNU C from its first line. va_start, va_arg, va_end and va_copy
            are predefined macros over __builtin_*, so a variadic function
            needs no include at all.
      oracle: glibc. vsprintf(buf, fmt, ap) hands our va_list to a library
            compiled years ago by another compiler, which walks it with its
            own va_arg. One field wrong and it reads memory that is not our
            arguments. Nothing we could assert about our own structures is
            worth as much.
            And in the other direction: va_arg's overflow branch cannot be
            reached by any program this compiler builds, because its call
            path stops at six arguments. So gcc makes the call, gcc
            compiles a main that passes nine arguments to a variadic
            function we compiled, and they are linked together. Their
            convention, our prologue, our va_arg.
      proves: 7 planted defects, 6 caught, 1 alive on purpose (va_list as a
            struct, not an array of one, indistinguishable here
            because every aggregate is already address-valued, and it stops
            being indistinguishable at item 13).
- [x] 12 bitfields
      why: `sqlite3.c` uses them, and they are the largest typical `c-testsuite`
           failure cluster. More immediately: every remaining system header
           stopped on `struct _IO_FILE`.
      done: layout packs into storage units, `:0` breaks one, unnamed fields
            are padding, and a field that will not fit whole in the open unit
            starts a new one instead of straddling. Reading shifts the field
            to the top of the register and arithmetic-shifts it back, which
            is what sign-extends an odd width for free. Writing is
            read-modify-write, and the value of the assignment is the field
            as it reads back, not the value offered.
      proves: 7 planted defects, 7 caught: after one of them forced a
            better case. "fields always share a unit" survived at first
            because the only test was sizeof, and a straddled field gives
            the same size; it takes reading the value back to see that the
            top bits of a field at bit 30 of a four-byte unit are not in the
            word that gets loaded.
      found: two bugs in new code that the suite alone would not have caught.
            A 32-bit-wide field's mask does not fit in an AND immediate, and
            GAS promotes `mov $imm64` to movabs while silently having no
            such escape for `and`. And `mask << offset` with an all-ones
            signed mask is undefined; `make test-asan`, added two commits
            ago, reported it on the first 64-bit field anyone wrote.
- [x] 12b type qualifiers, and a 64-bit literal
      done: const, volatile, restrict, __restrict, inline and _Noreturn parse
            and are ignored, in declspec and after every `*`. There is no
            const-checking here and the README says so instead of implying
            otherwise. <string.h> and <ctype.h> compile and run as a result.
            Number literals went through strtol, which saturates at LONG_MAX,
            so 18446744073709551615 silently became 9223372036854775807.
            strtoul: a literal is never negative here, since unary minus is
            its own node.
- [x] 13 function pointer declarators
      why: **measured, not planned.** `cc.c` has exactly two things left that
           it cannot compile about itself, and this is one of them:
           `Token *(*builtin)(Token *tok)` as a struct member and as a
           parameter. It is also the last thing standing between the
           compiler and `<stdlib.h>`, which stops at
           `void (*__func)(void)`. Needs recursive declarator parsing:
           the name can be wrapped in parentheses and the suffixes bind
           outward from it.
      done: declarator() skips the parenthesised part, parses the suffix
            that follows it to build the type the inside should be handed,
            then goes back and reads the inside against that. Two passes
            over a handful of tokens; the alternative is threading a
            half-built type through the recursion.
            A call became an operator in postfix() instead of something
            only a bare name can do, and that is what makes "m->builtin(tok)"
            writable: the shape cc.c uses. A function joined arrays and
            aggregates in is_address_valued(), because its value is its
            address; without that, loading a function designator emits
            mov (%rax), %rax and dereferences its first instruction.
      found: three bugs, all mine, all in the new code.
            The callee is pushed after the arguments and so is on top, but
            the arguments were popped first, putting the callee in an
            argument register and the last argument in %r10, then calling
            whatever the argument happened to be.
            Replacing consume("(") with peek("(") in primary() left the
            cursor on the paren for every direct call. 45 cases failed, and
            an earlier run of the suite had reported 291 passed against a
            binary that had not been rebuilt. Rebuild, then read.
      proves: 5 planted defects, 5 caught.
- [x] 14 compound literals with designated initialisers
      why: the other one. `#define PRIM(k, sz, uns) &(Type){.kind = (k), ...}`
           is how `cc.c` builds its ten primitive types, and it is C99 rather
           than GNU C, so there is no way around it short of rewriting the
           source to avoid it, which would be arranging for the test to
           pass instead of passing it.
- [x] 15 struct arguments and returns: the SysV eightbyte classification
      why: the nastiest part of the ABI. Currently rejected with an explicit
           error at the definition and the call, not miscompiled,
           which is the right interim answer, and `t/run.sh` asserts the
           refusal with `unimpl()`.
      note: **moved out of the self-host path.** `cc.c` passes zero structs
            by value and returns zero, so this is not a prerequisite for the
            fixpoint. It was assumed to be one. It is still wanted for
            `c-testsuite` and for `sqlite3.c`, which is where it belongs.
      done: up to 16 bytes, in one or two integer registers, both
            directions. The classification collapses to a size check
            because there are no floating-point registers here, with one
            hole closed by hand: float and double are declarable, so a
            struct containing one would be SSE class to anyone else and is
            refused, not passed in the wrong register file.
            A ragged tail is read and written at its real width. Loading a
            full eightbyte for a 12-byte struct reads four bytes belonging
            to the next local; writing one tramples them.
            Returns arrive in rax:rdx and are parked in a hidden local,
            because every aggregate here is addressed, not held.
            Three things fell out that had nothing to do with the ABI: an
            aggregate argument must not be wrapped in a cast (no conversion
            applies, and the cast made it a non-lvalue exactly where its
            address was needed); "struct P a = f();" was unsupported while
            "a = f();" worked, no distinction C makes; and
            gen_addr() falls back to gen_expr() for any aggregate, which is
            what lets id(id(a)) work, with an explicit lvalue whitelist
            added at parse time so "f() = x" is still refused, because
            being addressable and being assignable are different questions
            and codegen was about to answer both with the first.
      limit: >16 bytes stays refused. That is the MEMORY class, it needs
            arguments on the stack, and this compiler has none: the same
            gap that caps every call at six arguments.
      proves: 7 planted defects, 7 caught. The oracle is a program whose
            driver GCC compiles: gcc generates the calls into our functions
            and receives our returns, and we call and receive gcc's. A
            calling convention can be self-consistently wrong, our caller
            agreeing with our callee proves nothing, so only something
            that was not told what we decided can say whether it is right.
      done: up to 16 bytes, in one or two integer registers, both directions.
            The classification collapses to a size check because there are
            no floating-point registers here, with one hole closed by
            hand: float and double are declarable, so a struct containing
            one would be SSE class to anyone else and is refused rather
            than passed in the wrong register file.
            A ragged tail is read and written at its real width. Loading a
            full eightbyte for a 12-byte struct reads four bytes that
            belong to another local, and writing one tramples them.
            Returns arrive in rax:rdx and are parked in a hidden local,
            because every aggregate here is addressed, not held.
            Three things fell out that had nothing to do with the ABI:
            an aggregate argument must not be wrapped in a cast (it has no
            conversion to apply, and the cast made it a non-lvalue just as
            its address was needed); "struct P a = f();" was unsupported
            while "a = f();" worked, no distinction C makes;
            and gen_addr() now falls back to gen_expr() for any aggregate,
            which is what lets id(id(a)) work, with an explicit lvalue
            whitelist added at parse time so that "f() = x" is still
            refused, since being addressable and being assignable are
            different questions.
      note: >16 bytes stays refused. It needs stack arguments, which this
            compiler does not have at all (it stops at six), and returns
            need the hidden-pointer protocol.
      proves: 7 planted defects, 7 caught. The oracle is a program gcc
            compiles the driver for: gcc generates the calls into our
            functions and receives our returns, and we call and receive
            gcc's, because a passing convention can be self-consistently
            wrong, and only something that was not told what we decided can
            say otherwise.
- [x] 16 integer promotion done properly, replacing the current approximation
      note: the relational half turned out to be a real bug and was fixed
            early, as 08b. This is the arithmetic half.
      done: the type system and the code generator now share one rule. They
            disagreed: add_type computed the result of an arithmetic
            operation as "long if either side is long, else int" and
            dropped unsignedness entirely, while codegen picked its
            instruction from op_unsigned(). So "u - 1" on an unsigned u
            produced exactly the right bits and then called itself an int,
            and the comparison after it went signed. op_unsigned() moved up
            out of codegen and arith_result() is built on it, so there is
            one answer, not two.
            The conversions are now conversions. Computing only a result
            type leaves a narrow operand to be widened by whatever the load
            instruction happens to do, and a plain 32-bit load
            zero-extends: "u + i" with an unsigned long and an int of -1
            added 0x00000000FFFFFFFF, not -1. Both operands are cast
            to the result type in the tree.
            Unary plus promotes. It was returning its operand untouched, so
            sizeof(+a) on a short was 2.
      proves: 368 generated expressions compared with gcc, every operand
            type against every integer operator, plus a separate shift
            sweep because shifts are not symmetric and their count must
            stay in range. Each one is checked on three things at once:
            the sign of the result, whether it is zero, and its sizeof.
            Checking only the value would miss a wrong type that computes
            the right number, which is precisely the bug this item was.
            5 planted defects, 5 caught, but only after the shift sweep
            was added. The mutation that stopped promoting a shift's left
            operand survived everything else, because the matrix had no
            shift operators in it.
- [x] 17 self-host: `cmp cc2 cc3` in CI: `make self-host`
      done: five bugs had to be fixed, and not one was found by looking.
            Three came from the compiler failing on its own source:
              new_cast() never typed its operand, so "(long)f()" handed
              codegen a NULL type. Every other caller passed something
              already typed, so casting a CALL crashed the compiler.
              A prototype appearing AFTER a definition cleared
              is_definition and deleted the definition from the output.
              Only the linker noticed.
              Pointer scaling multiplied in 32 bits, so a negative index
              was zero-extended and p[-1] read four gigabytes forward.
              verror_at() walks backwards that way to find the start of a
              line, so every error message in the self-hosted compiler
              segfaulted.
            Two more came from cc2 failing cases cc passed:
              every integer literal was typed int, so "1L" was an int;
              and a shift took its width from BOTH operands, so "1L << 32"
              was emitted as a 32-bit shift and x86 masked the count to 5
              bits, giving 1.
      proves: the fixpoint, plus cc2 passing all 302 + 64 cases and building
            an ordinary program through real headers.
      proves: the fixpoint invariant, from here to the end of the project

- [x] 12c #include_next, and the compiler's answers about its own types
      done: gcc's limits.h reaches glibc's by #include_next, two files of
            the same name in different directories, each pulling in the
            other, so a file records which search path found it and the
            next search resumes after that. Searching from the start finds
            the same file again and recurses.
            gcc's stdint-gcc.h asks the compiler what its integer types are
            called instead of hardcoding them, so the __INT_LEAST8_TYPE__
            family and friends are predefined with the LP64 answers.
      state: <stddef.h> <stdarg.h> <string.h> <ctype.h> <stdint.h>
            <limits.h> <stdbool.h> all compile from /usr/include and run.
            <stdio.h> stops at a function typedef and <stdlib.h> at
            "typedef float _Float32", the first time the no-float
            decision has actually cost anything.

- [x] 12d real headers, and floating point as a declarable type
      why: <stdio.h> and <stdlib.h> both stopped at "typedef float _Float32",
           and refusing the TYPE blocks headers that never do arithmetic.
      done: float, double and long double parse, size correctly (4, 8, 16)
            and take part in declarations. Producing a VALUE of one is a
            named error in load() and store(), the only place a
            floating-point register would be needed. Declarable, not
            operable, and the suite asserts both halves.
            Function typedefs ("typedef int handler(void *)") parse; only
            the return type is kept, since nothing calls through one.
            The struct-by-value refusal moved from the declaration to the
            definition and the call. glibc declares fopencookie taking a
            cookie_io_functions_t by value, and nothing that merely includes
            <stdio.h> is asking to call it.
      state: <stdio.h> compiles, and a program using printf, sprintf and
            strcmp through real glibc headers runs. <stdlib.h> is the last
            one and stops at "void (*__func)(void)", function pointer
            declarators, which need recursive declarator parsing.
      found: t/run.sh gained unimpl(), the opposite of refuse(): gcc must
            ACCEPT the program and we must reject it cleanly. The failure
            mode of an unimplemented feature is not a missing error. It is
            silently generating wrong code for something that looked close
            enough, and four documented gaps now assert they refuse instead.

Milestone 1 is complete. The compiler self-hosts, and `make self-host`
asserts both halves of that: the fixpoint, and the stage-2 compiler passing
the whole suite.

### Milestone 2: external conformance

- [ ] 18 `c-testsuite` runner, publishing the pass rate with the failures named
- [ ] 19 csmith differential fuzzing against gcc and clang
      note: csmith has to be fetched and built, and nothing in this project
            downloads and runs third-party code without being asked. 19b
            takes the same approach with a generator written here: the
            generator being ours costs nothing, because the ORACLE is still
            gcc and clang, and that is the part that has to be independent.
- [x] 19b a random program generator, differentially tested against gcc and
      clang, needing nothing fetched
      done: t/gen.py and t/fuzz.sh, plus `make fuzz` and a 20-seed smoke
            run inside `make test`. The hard part is
            emitting C that is DEFINED: arithmetic in unsigned long so
            overflow wraps, every divisor `| 1`, every shift count `& 63`,
            every variable initialised before it is read. Without that the
            references disagree with each other and a difference against us
            proves nothing.
            Seeds where gcc and clang disagree are skipped AND counted, so
            the denominator cannot shrink quietly.
      proves: 7 planted defects in codegen, caught by between 5 and 44 of
            100 seeds. A fuzzer that has never been watched to fail is a
            fuzzer that always passes.
            Three of those started at zero or one, and the reasons are the
            substance of this item:
            generating a construct is not exercising it, structs and
            narrow types were emitted and then their values discarded, so
            every construct now folds into a running accumulator and
            "struct in one register" went from 1 of 100 to 18;
            a signed type needs negative values, narrow variables were
            masked to 0..127, so a signed load was indistinguishable from
            an unsigned one at 0 of 100, and they now land in -64..63;
            and narrowing cannot test a conversion at the call, because the
            callee truncates into its own slot and is right either way, so
            every narrow variable is also passed to a function taking long.
            That last one is the same vacuity as g(300) in item 08, found
            again in a different disguise.
      limit: it cannot find a signed-overflow bug, because it never
            generates signed overflow. Stated.
      found: two bugs in the generator itself on the first run, both caught
            because all three compilers rejected the output,
            not disagreeing about it: a literal %% in a non-format string,
            and variables declared in a nested block escaping their scope
            into the final fold.
      why: finds the cases I would never think to write. Every expectation I have
           written by hand in this project that was wrong, five so far, would have
           been caught here for free.
- [ ] 20 fix what 18 and 19 surface, one commit per root cause
- [ ] 21 compile `sqlite3.c`, then run sqlite's own test suite against the result

### Milestone 3: only after the above is green

- [ ] 22 replace the stack machine with a register allocator; keep the fixpoint green
      note: two cheap cases are already taken, and they are only worth 3%:
            a leaf right operand goes straight into %rdi, and a call whose
            arguments are all leaves loads each into its own register. On a
            frozen 4,677-line input that is 38,946 instructions down to
            37,646, against gcc -O0's 15,925. The remaining 2.4x is the
            allocator, not peepholes.
            Two things learned while taking them. Staging an argument
            through a scratch register costs the same two instructions the
            push/pop pair did and destroys whatever is already in that
            register, loading directly into the target is the only
            version that wins. And a no-op cast is not free: funcall was
            wrapping every argument in a cast to the type it already had,
            which hid the operand from every later question about whether
            it was simple.
- [ ] 23 emit ELF objects directly, dropping the `gcc` dependency (see ../ld)

## Open questions

- ~~`long double`: planning to alias it to `double` and document the lie.~~
  Resolved, and not the way this said. `long double` is declarable with its
  real 16-byte size and refuses to be operated on, so there is no lie left to
  document. The differential suite is what settled it. The moment the type
  became declarable, `sizeof(long double)` was a `check` asserting 8 against
  gcc and clang both saying 16, and a deliberate known-wrong answer turns out
  to be very hard to keep once something outside the repo gets a vote on it.
- Struct assignment currently emits an unrolled byte copy, so a 4KB struct would
  produce 8K instructions. Fine today; needs `rep movsb` or a loop before
  anything real is compiled.
- Compound assignment (`a += b`) evaluates its lvalue twice, which is wrong for a
  side-effecting lvalue like `*p++ += 1`. Needs a hidden temporary. Nothing in
  `t/` trips it, so it is marked `shortcut:` in the source, not fixed.
- `switch` compiles to a linear chain of compares. A jump table would be better
  for a dense switch and worse for a sparse one. No case in `t/` is large enough
  for the difference to be measurable yet.
