#!/bin/sh
# The real oracle: for every case in run.sh, compare our exit status against
# gcc's AND clang's. No hand-written expected values, so a wrong expectation in
# run.sh cannot hide a bug (and a bug cannot hide behind a wrong expectation --
# both happened on the first run of run.sh).
#
# Reuses run.sh's cases rather than keeping a second list in sync: we source it
# with `check` redefined to mean "diff against the reference compilers".
cd "$(dirname "$0")/.." || exit 1

pass=0; fail=0; skip=0

run_ref() {   # run_ref <compiler> <srcfile>; echoes exit status, or "x" if it won't build
  if "$1" -w "$2" -o t/ref.bin 2>/dev/null; then
    ./t/ref.bin; printf '%s' "$?"
  else
    printf 'x'
  fi
}

check() {
  # $1 is run.sh's expected value; ignored on purpose -- gcc and clang are the
  # authority here, not us.
  src="$2"
  printf '%s' "$src" > t/dt.c

  g=$(run_ref gcc t/dt.c)
  c=$(run_ref clang t/dt.c)

  if [ "$g" = x ] || [ "$c" = x ]; then
    skip=$((skip + 1)); return                 # not valid C, or uses an extension
  fi
  if [ "$g" != "$c" ]; then
    skip=$((skip + 1))                         # gcc and clang disagree: UB, not our problem
    printf 'SKIP (gcc=%s clang=%s) %s\n' "$g" "$c" "$src"
    return
  fi

  if ! ./cc t/dt.c -o t/dt.bin 2>t/dt.err; then
    fail=$((fail + 1))
    printf 'FAIL (we reject, gcc+clang accept)  %s\n' "$src"
    sed 's/^/        /' t/dt.err
    return
  fi
  ./t/dt.bin; ours=$?

  if [ "$ours" = "$g" ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    printf 'FAIL  %s\n        gcc+clang=%s  cc=%s\n' "$src" "$g" "$ours"
  fi
}

# Pull in just the case lines. Sourcing run.sh wholesale would redefine check()
# back to its own version and silently defeat the whole point of this file.
grep '^check ' t/run.sh > t/cases.sh
. ./t/cases.sh
rm -f t/cases.sh

printf '%d agree with gcc+clang, %d disagree, %d skipped\n' "$pass" "$fail" "$skip"
rm -f t/dt.c t/dt.bin t/dt.err t/ref.bin
[ "$fail" = 0 ]
