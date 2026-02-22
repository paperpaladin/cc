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
seed=$FIRST
while [ "$seed" -le "$LAST" ]; do
  python3 t/gen.py "$seed" > "$TMP/p.c"

  if ! gcc -w "$TMP/p.c" -o "$TMP/gcc.bin" 2>/dev/null; then
    skipped=$((skipped + 1)); seed=$((seed + 1)); continue
  fi
  "$TMP/gcc.bin" || true; g=$?
  if ! clang -w "$TMP/p.c" -o "$TMP/clang.bin" 2>/dev/null; then
    skipped=$((skipped + 1)); seed=$((seed + 1)); continue
  fi
  "$TMP/clang.bin" || true; c=$?

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
  "$TMP/our.bin" || true; o=$?

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
[ "$disagree" -eq 0 ]
