#!/bin/sh
# c-testsuite: 220 small C programs written by other people.
#
# Every other oracle here compares cc against gcc and clang on programs
# this repository wrote. This one is a corpus nobody here chose, which is
# a different question: not "do we agree about our own test cases" but
# "how much of the language do we actually have".
#
# The corpus is NOT vendored. Its per-test metadata records where each
# case came from (scc, gcc's torture suite, others) and gives no licence
# for the individual files, so redistributing them inside this repository
# would be making a claim about rights nobody here holds. It is fetched
# instead, pinned to one commit and checked by sha256, so it cannot
# change underneath a result.
#
# Offline, this skips loudly. It is one oracle among several here and a
# missing one should not stop the suite, but it must not look like it
# ran either.
set -u
cd "$(dirname "$0")/.."

COMMIT=5c7275656d751de0e68b2d340a95b5681858ed07
SHA256=010008bf4b5671f947ae7e8d693a1c959a4a4d795fa7227c0abb1f95de45f7e7
URL="https://codeload.github.com/c-testsuite/c-testsuite/tar.gz/$COMMIT"
CACHE=scratch/c-testsuite
TARBALL=scratch/c-testsuite-$COMMIT.tar.gz
BASELINE=t/cts-baseline.txt

mkdir -p scratch

if [ ! -d "$CACHE" ]; then
	if [ ! -f "$TARBALL" ]; then
		command -v curl >/dev/null 2>&1 || {
			echo "SKIP c-testsuite: curl is not installed"; exit 0; }
		curl -sSL --max-time 120 -o "$TARBALL.part" "$URL" 2>/dev/null || {
			rm -f "$TARBALL.part"
			echo "SKIP c-testsuite: cannot reach the corpus (offline?)"
			exit 0; }
		mv "$TARBALL.part" "$TARBALL"
	fi
	got=$(sha256sum "$TARBALL" | cut -d' ' -f1)
	if [ "$got" != "$SHA256" ]; then
		echo "FAIL c-testsuite: tarball sha256 is $got, expected $SHA256" >&2
		echo "  the pin names one commit; a different payload under that" >&2
		echo "  name is the thing a pin exists to catch." >&2
		rm -f "$TARBALL"
		exit 1
	fi
	mkdir -p "$CACHE"
	tar -xzf "$TARBALL" -C "$CACHE" --strip-components=1 || {
		echo "FAIL c-testsuite: tarball did not extract" >&2; exit 1; }
fi

SRC="$CACHE/tests/single-exec"
[ -d "$SRC" ] || { echo "FAIL c-testsuite: $SRC is missing" >&2; exit 1; }

[ -x ./cc ] || make -s cc

work=$(mktemp -d "${TMPDIR:-/tmp}/cts.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT INT TERM

total=0
passed=0
failed=""

for src in "$SRC"/*.c; do
	name=$(basename "$src" .c)
	total=$((total + 1))
	exp="$src.expected"

	# A case passes when it compiles, links, runs, exits 0, and prints
	# exactly what the corpus says it prints.
	#
	# The two ways of not passing are not the same thing and the output
	# must never merge them. "refused" means cc declined to compile it:
	# an honest answer, and not conformance. "MISCOMPILED" means cc
	# accepted the program and produced a binary that then crashed or
	# printed the wrong thing, which is the failure this compiler is
	# built to avoid. The first is a gap. The second is a bug.
	if ! ./cc "$src" -o "$work/$name" >"$work/$name.cc" 2>&1; then
		failed="$failed $name:refused"
		continue
	fi
	"$work/$name" >"$work/$name.out" 2>/dev/null
	st=$?
	if [ "$st" -gt 128 ]; then
		failed="$failed $name:MISCOMPILED-signal$((st - 128))"
		continue
	fi
	if [ "$st" -ne 0 ]; then
		failed="$failed $name:MISCOMPILED-exit$st"
		continue
	fi
	if [ -f "$exp" ] && ! cmp -s "$exp" "$work/$name.out"; then
		failed="$failed $name:MISCOMPILED-stdout"
		continue
	fi
	passed=$((passed + 1))
done

printf 'c-testsuite: %d of %d pass (%d%%)\n' \
       "$passed" "$total" "$((passed * 100 / total))"

# The gate is no regression, not a pass rate. cc implements a subset of
# C on purpose, so most failures here are known and expected; what must
# never happen is a case that used to pass and now does not.
#
# The baseline is checked in BOTH directions. A newly passing case has
# to be removed from it, or the file rots into a list of things nobody
# has rechecked, and the gate quietly weakens every time cc improves.
now=$(printf '%s\n' $failed | sed 's/:.*//' | sort -u)
if [ ! -f "$BASELINE" ]; then
	printf '%s\n' "$now" > "$BASELINE"
	echo "  wrote a new baseline of $(printf '%s\n' "$now" | grep -c .) known failures"
	exit 0
fi
was=$(grep -v '^#' "$BASELINE" | grep . | sort -u)

# Files rather than process substitution: this runs under /bin/sh and
# "<(...)" is a bashism that dash rejects outright. The first version
# had it, and the only reason it was caught is that "sh -n" was run on
# the script before the branch containing it was ever reached.
printf '%s\n' "$was" > "$work/was"
printf '%s\n' "$now" > "$work/now"
regressed=$(comm -13 "$work/was" "$work/now")
fixed=$(comm -23 "$work/was" "$work/now")

rc=0
if [ -n "$regressed" ]; then
	echo "  REGRESSED, these passed before:"
	printf '%s\n' "$regressed" | sed 's/^/    /'
	rc=1
fi
if [ -n "$fixed" ]; then
	echo "  now passing and still listed as known failures:"
	printf '%s\n' "$fixed" | sed 's/^/    /'
	echo "  remove them from $BASELINE"
	rc=1
fi
[ "$rc" -eq 0 ] && echo "  no regressions against $BASELINE"

# Name the failures by kind, so the number above is readable rather than
# just a score.
echo "  by kind:"
printf '%s\n' $failed | sed 's/.*://;s/signal[0-9]*/signal/;s/exit[0-9]*/exit nonzero/' |
	sort | uniq -c | sort -rn | sed 's/^/    /'

# A miscompilation is worth naming individually. There should not be
# many, and a list of three is read where a count of three is not.
mis=$(printf '%s\n' $failed | grep MISCOMPILED | sed 's/:/ /')
if [ -n "$mis" ]; then
	echo "  accepted and then wrong, which is the bad kind:"
	printf '%s\n' "$mis" | sed 's/^/    /'
fi

exit $rc
