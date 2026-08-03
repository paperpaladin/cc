#!/bin/sh
# Differential fuzzing: random programs, judged by gcc and clang.
#
# t/gen.py emits a program whose value is a fact rather than an opinion --
# see its header for the rules that keep it free of undefined behaviour. This
# runs a range of seeds through all three compilers and reports any seed where
# we disagree with a reference the other two agree on.
#
# Why both references and not just gcc: if gcc and clang disagree with EACH
# OTHER, the program has undefined behaviour the generator failed to exclude,
# and a difference proves nothing about us. Those seeds are skipped and
# counted, because a silently shrinking denominator is how a fuzzer stops
# meaning anything.
#
#   ./t/fuzz.sh [FIRST] [LAST]     default 1..200
#   CCBIN=./cc-asan ./t/fuzz.sh    fuzz the sanitized build
set -eu
cd "$(dirname "$0")/.."
CCBIN=${CCBIN:-./cc}
FIRST=${1:-1}
LAST=${2:-200}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

agree=0; disagree=0; skipped=0

# A census of what the generator actually emitted, counted from the
# sources themselves rather than from what the generator is believed to
# emit. "0 disagree" over a range that never produced a switch is not
# evidence about switch, and the only way to tell those apart is to
# count. The floors below are asserted at the end: a construct that
# stops being generated, because a probability was retuned or a branch
# became unreachable, turns this red instead of quietly shrinking what
# is being fuzzed.
#
# This was prompted by widening the generator and finding the new
# constructs appeared in one program in thirty. The fuzzer reported
# 300 agree either way.
c_enum=0; c_switch=0; c_compound=0; c_struct=0; c_narrow=0; c_loop=0
seed=$FIRST
while [ "$seed" -le "$LAST" ]; do
  python3 t/gen.py "$seed" > "$TMP/p.c"

  grep -qE '\.f = ' "$TMP/p.c"      && c_enum=$((c_enum + 1))
  grep -qE 'switch \(' "$TMP/p.c"   && c_switch=$((c_switch + 1))
  grep -qE '(\+=|-=|\*=|\^=|\|=|&=) \(' "$TMP/p.c" && c_compound=$((c_compound + 1))
  grep -qE 'fold\(' "$TMP/p.c"      && c_struct=$((c_struct + 1))
  grep -qE 'narrow\(' "$TMP/p.c"    && c_narrow=$((c_narrow + 1))
  grep -qE 'for \(int ' "$TMP/p.c"  && c_loop=$((c_loop + 1))

  if ! gcc -w "$TMP/p.c" -o "$TMP/gcc.bin" 2>/dev/null; then
    skipped=$((skipped + 1)); seed=$((seed + 1)); continue
  fi
  g=0; "$TMP/gcc.bin" || g=$?
  if ! clang -w "$TMP/p.c" -o "$TMP/clang.bin" 2>/dev/null; then
    skipped=$((skipped + 1)); seed=$((seed + 1)); continue
  fi
  c=0; "$TMP/clang.bin" || c=$?

  # `cmd || true; v=$?` CAPTURES THE STATUS OF `true`, NOT OF cmd.
  #
  # That is how these three lines were written, so g, c and o were all 0 on
  # every seed, for the life of this file. `[ "$g" != "$c" ]` below could
  # never fire -- the skip counter it guards was unreachable -- and
  # `[ "$o" = "$g" ]` was `0 = 0`, so every seed was counted as agreeing and
  # a wrong answer from cc was undetectable. The only defect this script
  # could still catch was cc REFUSING a program gcc and clang accept, which
  # is tested by the compiler's own status and was correct.
  #
  # `v=0; cmd || v=$?` is the right spelling and was already in use next
  # door at t/cpp.sh:369. The generator returns `result % 251`, so almost
  # every seed exits non-zero: there was no shortage of signal, only a
  # harness that threw it away.

  # The references must agree before their answer is worth anything.
  if [ "$g" != "$c" ]; then
    skipped=$((skipped + 1))
    printf 'skip  seed %s: gcc=%s clang=%s (undefined behaviour the generator let through)\n' "$seed" "$g" "$c"
    seed=$((seed + 1)); continue
  fi

  if ! "$CCBIN" "$TMP/p.c" -o "$TMP/our.bin" 2> "$TMP/err"; then
    disagree=$((disagree + 1))
    printf 'FAIL  seed %s: we refused a program gcc and clang both accept\n' "$seed"
    sed 's/^/        /' "$TMP/err" | head -3
    cp "$TMP/p.c" "t/fuzz-fail-$seed.c"
    seed=$((seed + 1)); continue
  fi
  o=0; "$TMP/our.bin" || o=$?

  if [ "$o" = "$g" ]; then
    agree=$((agree + 1))
  else
    disagree=$((disagree + 1))
    printf 'FAIL  seed %s: we return %s, gcc and clang both return %s\n' "$seed" "$o" "$g"
    cp "$TMP/p.c" "t/fuzz-fail-$seed.c"
    printf '      kept as t/fuzz-fail-%s.c\n' "$seed"
  fi
  seed=$((seed + 1))
done

printf '\n%d agree, %d disagree, %d skipped (seeds %s..%s)\n' \
  "$agree" "$disagree" "$skipped" "$FIRST" "$LAST"
# A floor on the denominator. The header above says "a silently shrinking
# denominator is how a fuzzer stops meaning anything" -- and then printed
# the denominator without asserting it. If the generator started emitting
# programs gcc rejects, every seed would take the `skipped` path and this
# script would exit 0 having compared nothing.
if [ "$agree" -eq 0 ]; then
  # FIRST and LAST, not to and from: those names do not exist, and `set -u`
  # does not catch it because arithmetic expansion treats an unset name as 0,
  # so this printed "0 skipped of 1" whatever the range was. The gate itself
  # was right; only the message on the never-taken path was wrong.
  printf 'FAIL  no seed produced a comparison: %s skipped of %s\n' \
    "$skipped" "$((LAST - FIRST + 1))"
  exit 1
fi
printf 'reached: %s enum bit-field, %s switch, %s compound assign, %s struct by value, %s narrow call, %s loop (of %s seeds)\n' \
  "$c_enum" "$c_switch" "$c_compound" "$c_struct" "$c_narrow" "$c_loop" \
  "$((LAST - FIRST + 1))"

# Floors, not targets. Each is well under what the generator produces
# today, so this fires when a construct stops being generated at all
# rather than on ordinary variation. A range too short to clear them
# is not a failure of the compiler and says so.
span=$((LAST - FIRST + 1))
if [ "$span" -ge 40 ]; then
  short=""
  [ "$c_enum" -lt 5 ]     && short="$short enum-bitfield($c_enum)"
  [ "$c_switch" -lt 5 ]   && short="$short switch($c_switch)"
  [ "$c_compound" -lt 5 ] && short="$short compound-assign($c_compound)"
  [ "$c_struct" -lt 5 ]   && short="$short struct-by-value($c_struct)"
  [ "$c_narrow" -lt 5 ]   && short="$short narrow-call($c_narrow)"
  [ "$c_loop" -lt 5 ]     && short="$short loop($c_loop)"
  if [ -n "$short" ]; then
    printf 'FAIL  the generator stopped emitting:%s\n' "$short"
    printf '      agreement over a range that never generated a construct is\n'
    printf '      not evidence about that construct.\n'
    exit 1
  fi
fi

[ "$disagree" -eq 0 ]
