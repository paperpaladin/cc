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
    # Said out loud. This was a bare `skip=$((skip+1)); return`, so the
    # reference compilers refusing a case removed it from the oracle with no
    # trace -- and five of the seven skips are every va_list/va_start/va_arg
    # case, which gcc rejects without <stdarg.h> because va_list is a cc
    # builtin. README.md calls the variadic comparison the strongest oracle
    # available, and it was silently not running. The count went 2 -> 7 at
    # some point with nothing to say so.
    skip=$((skip + 1))
    printf 'SKIP (gcc=%s clang=%s, reference will not build it) %s\n' "$g" "$c" "$src"
    return
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

# Cases whose source spans more than one line are written as check_src and
# are invisible to the line-at-a-time extraction above. Counting them here
# rather than leaving them out quietly: a harness that covers less than it
# appears to is the failure mode this project keeps running into, and the
# number is cheap to print.
notrun=$(grep -c '^check_src ' t/run.sh)

printf '%d agree with gcc+clang, %d disagree, %d skipped\n' "$pass" "$fail" "$skip"
printf '%d multi-line case(s) in t/run.sh are not differential-tested\n' "$notrun"
rm -f t/dt.c t/dt.bin t/dt.err t/ref.bin

# The cases come from `grep '^check ' t/run.sh`, so anything that breaks that
# convention -- reindenting, renaming the helper, wrapping cases in a function
# -- silently deletes this file's entire subject. The gate was `[ "$fail" = 0 ]`
# alone, which zero cases satisfy: pointing the grep at a pattern that matches
# nothing gave "0 agree with gcc+clang, 0 disagree, 0 skipped" and exit 0.
# This is the oracle the README calls the one that matters, so it has to
# notice when it is measuring nothing.
#
# The floor is a long way below the 282 that run today, because the number
# moves with every case added and this is here to catch an empty sweep, not
# to pin a count.
CASES_FLOOR=200
if [ "$((pass + fail))" -lt "$CASES_FLOOR" ]; then
	printf 'differential: only %d case(s) were compared, under the %d floor.\n' \
		"$((pass + fail))" "$CASES_FLOOR" >&2
	printf '  The cases are extracted with `grep "^check " t/run.sh`; if that\n' >&2
	printf '  convention changed, this harness has no subject.\n' >&2
	exit 1
fi

[ "$fail" = 0 ]
