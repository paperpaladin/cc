#!/bin/sh
# The fixpoint. cc compiles its own source to make cc2; cc2 compiles the same
# source to make cc3; cc2 and cc3 must be byte-identical.
#
# What that proves and what it does not:
#
# It proves the compiler is a FIXED POINT of itself -- that whatever cc2 does
# differently from cc, it does consistently enough to reproduce itself
# exactly. Any miscompilation in a path cc.c exercises shows up as cc3
# differing from cc2, because cc2 was built by a compiler that got it right
# and cc3 by one that did not.
#
# It does NOT prove cc2 is correct. A compiler that miscompiles itself in a
# self-consistent way still reaches a fixpoint, and this one did: at the
# first green `cmp cc2 cc3` here, cc2 still failed 15 cases of t/run.sh. The
# fixpoint said "stable" and the suite said "wrong", and the suite was right.
# So the stage-2 compiler is run against the whole suite as well, and that is
# the assertion with teeth.
set -eu
cd "$(dirname "$0")/.."
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok   %s\n' "$1"; }
nok() { fail=$((fail+1)); printf 'FAIL %s\n' "$1"; }

stage() {
  name=$1; compiler=$2; out=$3
  if ! "$compiler" -S cc.c -o "$out.s" 2> "$TMP/err"; then
    nok "$name: cc.c did not compile"
    sed 's/^/       /' "$TMP/err" | head -5
    return 1
  fi
  # gcc assembles and links, the same as for any program this compiler emits.
  if ! gcc -static "$out.s" -o "$out" 2> "$TMP/lderr"; then
    nok "$name: the assembly did not assemble or link"
    sed 's/^/       /' "$TMP/lderr" | head -5
    return 1
  fi
  ok "$name"
  return 0
}

stage "stage 1: cc compiles cc.c"        ./cc        "$TMP/cc2" || exit 1
stage "stage 2: cc2 compiles cc.c"       "$TMP/cc2"  "$TMP/cc3" || exit 1

if cmp -s "$TMP/cc2" "$TMP/cc3"; then
  ok "the fixpoint: cc2 and cc3 are byte-identical"
else
  nok "the fixpoint: cc2 and cc3 differ"
  cmp "$TMP/cc2" "$TMP/cc3" 2>&1 | sed 's/^/       /' | head -3
fi

# Stage 2 has to be a working compiler, not merely a stable one.
if CCBIN="$TMP/cc2" ./t/run.sh > "$TMP/r2" 2>&1; then
  ok "cc2 passes t/run.sh  ($(tail -1 "$TMP/r2"))"
else
  nok "cc2 fails t/run.sh  ($(tail -1 "$TMP/r2"))"
  grep '^FAIL' "$TMP/r2" | head -5 | sed 's/^/       /'
fi

if CCBIN="$TMP/cc2" ./t/cpp.sh > "$TMP/c2" 2>&1; then
  ok "cc2 passes t/cpp.sh  ($(tail -1 "$TMP/c2"))"
else
  nok "cc2 fails t/cpp.sh  ($(tail -1 "$TMP/c2"))"
  grep '^FAIL' "$TMP/c2" | head -5 | sed 's/^/       /'
fi

# And it has to be able to build something that is not itself.
printf '#include <stdio.h>\nint main(void){ printf("%%d\\n", 6*7); return 0; }\n' > "$TMP/p.c"
if "$TMP/cc2" "$TMP/p.c" -o "$TMP/p" 2>/dev/null && [ "$("$TMP/p")" = "42" ]; then
  ok "cc2 builds an ordinary program through real headers"
else
  nok "cc2 cannot build an ordinary program"
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
