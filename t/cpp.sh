#!/bin/sh
# The preprocessor, judged by gcc -E -P and clang -E -P.
#
# Two independent preprocessors must agree with ours on the token stream. That
# is the whole oracle: no expectation in this file was written by hand, because
# every expectation written by hand in this project that turned out to be wrong
# was wrong in a way its author was confident about.
#
# Runs of whitespace are squeezed before comparing, because phase 4 only has to
# place a space where one is needed to keep two tokens apart and is otherwise
# unspecified. Where a space is *required* is not normalised away, and that is
# the interesting half: in C99 6.10.3.5's own example gcc and clang both emit
# "2 +(3,4)" where the standard's printed text has "2+(3,4)". They are right
# and the prose is loose -- a pp-number followed by '+' is exactly the shape of
# an exponent, so the two must not touch. We emit the space for that reason,
# not to match them.
set -eu
cd "$(dirname "$0")/.."
CC=${CCBIN:-./cc}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok   %s\n' "$1"; }
nok() { fail=$((fail+1)); printf 'FAIL %s\n' "$1"; }

norm() { tr -s ' \t' ' ' | sed 's/^ *//; s/ *$//' | grep -v '^$' || true; }

# A case is one file. It must preprocess identically under all three.
check() {
  name=$1; file=$2; shift 2
  if ! "$CC" -E "$file" "$@" > "$TMP/ours" 2> "$TMP/err"; then
    nok "$name: cc -E failed: $(head -1 "$TMP/err")"
    return
  fi
  gcc   -E -P "$@" "$file" > "$TMP/gcc"   2>/dev/null
  clang -E -P "$@" "$file" > "$TMP/clang" 2>/dev/null

  norm < "$TMP/ours"  > "$TMP/n_ours"
  norm < "$TMP/gcc"   > "$TMP/n_gcc"
  norm < "$TMP/clang" > "$TMP/n_clang"

  if ! cmp -s "$TMP/n_ours" "$TMP/n_gcc"; then
    nok "$name: differs from gcc"
    diff -u "$TMP/n_gcc" "$TMP/n_ours" | sed -n '3,12p' | sed 's/^/       /'
    return
  fi
  if ! cmp -s "$TMP/n_ours" "$TMP/n_clang"; then
    nok "$name: differs from clang"
    diff -u "$TMP/n_clang" "$TMP/n_ours" | sed -n '3,12p' | sed 's/^/       /'
    return
  fi

  # Idempotence. -E output is a token stream printed as text, and printing it
  # has to be lossless: reading it back must give the same tokens again. This
  # is what needs_space() exists for, and without the check the guarantee is
  # only a claim. It catches the whole class of bug where expansion puts two
  # tokens next to each other that silently lex as a third.
  cp "$TMP/ours" "$TMP/again.c"
  if ! "$CC" -E "$TMP/again.c" > "$TMP/ours2" 2>/dev/null; then
    nok "$name: its own -E output does not preprocess again"
    return
  fi
  if ! cmp -s "$TMP/ours" "$TMP/ours2"; then
    nok "$name: -E is not idempotent"
    diff -u "$TMP/ours" "$TMP/ours2" | sed -n '3,10p' | sed 's/^/       /'
    return
  fi
  ok "$name"
}

# Shorthand: a case written inline.
case_() { name=$1; shift; printf '%s\n' "$1" > "$TMP/c.c"; check "$name" "$TMP/c.c"; }

# ---- expansion must terminate --------------------------------------------
# The reason hide sets exist. Each of these loops forever under a naive
# "expand until nothing changes" implementation.
case_ 'self-reference stops'        '#define f(x) f(x)
f(1)'
case_ 'object self-reference stops' '#define foo (4 + foo)
foo'
case_ 'mutual recursion stops'      '#define a b
#define b a
a
b'
case_ 'indirect self-reference'     '#define f(x) g(x)
#define g(x) f(x)
f(1)'

# ---- object-like ----------------------------------------------------------
case_ 'object macro'         '#define N 3
N'
case_ 'empty body expands to nothing' '#define E
x E y'
case_ 'macro expands to a keyword'    '#define R return
R 0;'
case_ 'redefinition replaces'         '#define N 1
#define N 2
N'
case_ 'undef'                '#define N 1
#undef N
N'
case_ 'rescan expands further'        '#define A B
#define B C
#define C 9
A'

# ---- function-like --------------------------------------------------------
case_ 'function macro'       '#define SQ(x) ((x)*(x))
SQ(3)'
case_ 'two arguments'        '#define MAX(a,b) ((a)>(b)?(a):(b))
MAX(1,2)'
case_ 'nested call is one argument'  '#define F(a) [a]
#define G(a,b) a|b
F(G(1,2))'
case_ 'comma inside parens'  '#define F(a) [a]
F((1,2))'
case_ 'empty argument'       '#define F(a) [a]
F()'
case_ 'name without parens is not a call' '#define getc(f) BAD
int (*getc)(void);'
case_ 'argument expanded before substitution' '#define N 3
#define F(x) [x]
F(N)'
case_ 'parameter named like a keyword'  '#define F(int) (int)
F(7)'

# ---- # and ## -------------------------------------------------------------
case_ 'stringize'            '#define S(x) #x
S(a + b)'
case_ 'stringize normalises inner space' '#define S(x) #x
S( a   +   b )'
case_ 'stringize escapes quotes'  '#define S(x) #x
S("quoted")'
case_ 'stringize does not expand' '#define V 42
#define S(x) #x
S(V)'
case_ 'stringize after expansion' '#define V 42
#define S(x) #x
#define XS(x) S(x)
XS(V)'
case_ 'stringize empty'      '#define S(x) #x
S()'
case_ 'two stringizes in a row'   '#define S(x) #x
S(a)
S(b)'
case_ 'paste identifiers'    '#define CAT(a,b) a##b
CAT(ma,in)'
case_ 'paste makes an operator'   '#define P(a,b) a##b
P(+,+)'
case_ 'paste with empty lhs' '#define E
#define CAT(a,b) a##b
CAT(E,foo)'
case_ 'paste then rescan'    '#define CAT(a,b) a##b
#define XCAT(a,b) CAT(a,b)
#define AB 9
XCAT(A,B)'

# ---- variadic -------------------------------------------------------------
case_ 'variadic'             '#define L(f, ...) p(f, __VA_ARGS__)
L("x", 1, 2)'
case_ 'GNU comma deletion, empty'  '#define L(f, ...) p(f, ##__VA_ARGS__)
L("x")'
case_ 'GNU comma deletion, present' '#define L(f, ...) p(f, ##__VA_ARGS__)
L("x", 1)'
case_ 'GNU named variadic'   '#define L(f, args...) p(f, ##args)
L("x", 1)'

# ---- conditionals ---------------------------------------------------------
case_ 'if true'              '#if 1
yes
#endif'
case_ 'if false'             '#if 0
no
#endif'
case_ 'if arithmetic'        '#if 1 + 2 * 3 == 7
yes
#endif'
case_ 'ifdef / ifndef'       '#define ON 1
#ifdef ON
a
#endif
#ifndef ON
b
#endif
#ifndef OFF
c
#endif'
case_ 'defined, both spellings' '#define ON 1
#if defined(ON) && defined ON && !defined(OFF)
yes
#endif'
case_ 'undefined identifier is zero' '#if UNDEFINED
no
#else
yes
#endif'
case_ 'elif chain'           '#if 0
a
#elif 0
b
#elif 1
c
#else
d
#endif'
case_ 'elif not taken once included' '#if 1
a
#elif 1
b
#endif'
case_ 'nested, outer false'  '#if 0
#if 1
inner
#endif
outer
#endif'
case_ 'nested, outer true'   '#if 1
#if 0
inner
#else
inner_else
#endif
outer
#endif'
case_ 'macro used in #if'    '#define N 3
#if N > 2
yes
#endif'
case_ 'disabled branch is not parsed' '#if 0
this is not ) valid ( C at all
#endif
ok'

# ---- builtins -------------------------------------------------------------
case_ '__LINE__'             'int a = __LINE__;
int b = __LINE__;'
case_ '__LINE__ through a macro'  '#define HERE __LINE__
int a = HERE;'

# ---- #include -------------------------------------------------------------
mkdir -p "$TMP/inc"
cat > "$TMP/inc/one.h" <<'HDR'
#ifndef ONE_H
#define ONE_H
#define FROM_ONE 7
int one(void);
#endif
HDR
cat > "$TMP/inc/two.h" <<'HDR'
#include "one.h"
int two(void);
HDR
printf '#include "inc/one.h"\n#include "inc/one.h"\nint x = FROM_ONE;\n' > "$TMP/i1.c"
check 'include, guarded twice' "$TMP/i1.c"
printf '#include <one.h>\nint x = FROM_ONE;\n' > "$TMP/i2.c"
check 'include <> with -I' "$TMP/i2.c" "-I$TMP/inc"
printf '#include "two.h"\nint x = FROM_ONE;\n' > "$TMP/i3.c"
check 'nested include resolves relative to its own file' "$TMP/i3.c" "-I$TMP/inc"

# ---- real system headers --------------------------------------------------
# Not compared against gcc, and the reason is worth writing down: glibc's
# headers branch on __GNUC__, so gcc and this compiler are not reading the
# same source at all. gcc gets the GNU declarations with __attribute__ on
# them, we get the portable ones, and both are correct. A differential oracle
# stops meaning anything the moment the input depends on which compiler is
# asking, so the question here is only "does it come out whole", and
# t/run.sh asks the stronger one by compiling and running the result.
real_header() {
  name=$1; body=$2
  printf '%s\n' "$body" > "$TMP/rh.c"
  if "$CC" -E "$TMP/rh.c" > "$TMP/rh.i" 2> "$TMP/rh.err"; then
    ok "$name (preprocesses, $(wc -l < "$TMP/rh.i") lines)"
  else
    nok "$name: $(head -1 "$TMP/rh.err")"
  fi
}
real_header 'include <stddef.h>' '#include <stddef.h>
size_t n;
ptrdiff_t d;'
real_header 'include <stdarg.h>' '#include <stdarg.h>
int x;'
real_header 'include <string.h>'  '#include <string.h>'
real_header 'include <stdint.h>'  '#include <stdint.h>
int64_t v;'
real_header 'include <limits.h>, via include_next' '#include <limits.h>
int n = CHAR_BIT;'

# Line splicing is phase 2 and happens before anything is a token, which is
# why it can cut an identifier in half and still work.
case_ 'splice inside a macro body' '#define WIDE(a, \
             b) ((a)+(b))
WIDE(3,4)'
case_ 'splice inside an identifier' 'int ab\
c = 1;'
case_ 'splice keeps later line numbers' 'int a \
;
int l = __LINE__;'

# ---- the standard's own torture test --------------------------------------
# C99 6.10.3.5 EXAMPLE 3. Every published preprocessor is expected to get this
# exactly right, and it exercises rescanning, nested calls, ## and # together.
cat > "$TMP/std3.c" <<'STD'
#define x 3
#define f(a) f(x * (a))
#undef x
#define x 2
#define g f
#define z z[0]
#define h g(~
#define m(a) a(w)
#define w 0,1
#define t(a) a
#define p() int
#define q(x) x
#define r(x,y) x ## y
#define str(x) # x
f(y+1) + f(f(z)) % t(t(g)(0) + t)(1);
g(x+(3,4)-w) | h 5) & m
(f)^m(m);
p() i[q()] = { q(1), r(2,3), r(4,), r(,5), r(,) };
char c[2][6] = { str(hello), str() };
STD
check 'C99 6.10.3.5 example 3' "$TMP/std3.c"

# C99 6.10.3.5 EXAMPLE 5, the variadic one.
cat > "$TMP/std5.c" <<'STD'
#define debug(...) fprintf(stderr, __VA_ARGS__)
#define showlist(...) puts(#__VA_ARGS__)
#define report(test, ...) ((test)?puts(#test):printf(__VA_ARGS__))
debug("Flag");
debug("X = %d\n", x);
showlist(The first, second, and third items.);
report(x>y, "x is %d but y is %d", x, y);
STD
check 'C99 6.10.3.5 example 5' "$TMP/std5.c"

# ---- -D and -U on the command line ----------------------------------------
printf 'A B C\n' > "$TMP/d.c"
check '-D with a value'    "$TMP/d.c" -DA=1 -DB=2 -DC=3
check '-D without a value' "$TMP/d.c" -DA -DB -DC
printf '#define A 1\nA\n' > "$TMP/u.c"
check '-U removes a later #define' "$TMP/u.c" -UA

# ---- the result still compiles and runs -----------------------------------
# The token stream agreeing with gcc is necessary, not sufficient: the parser
# has to accept what comes out. This runs a preprocessed program end to end.
cat > "$TMP/run.c" <<'RUN'
#define N 5
#define SQ(x) ((x)*(x))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define JOIN(a,b) a##b
#if N > 3
#define BONUS 1
#else
#define BONUS 0
#endif
int JOIN(ma,in)(void) {
  int v = SQ(N) + MAX(2, 7) + BONUS;
  return v - 33;
}
RUN
if $CC "$TMP/run.c" -o "$TMP/run.bin" 2>"$TMP/err" && "$TMP/run.bin"; then
  got=$?
else
  got=$?
fi
gcc "$TMP/run.c" -o "$TMP/run.gcc" 2>/dev/null; "$TMP/run.gcc" || want=$?
want=${want:-0}
if [ "$got" -eq "$want" ]; then
  ok "a preprocessed program compiles and returns what gcc's does ($got)"
else
  nok "preprocessed program returned $got, gcc's returned $want"
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
