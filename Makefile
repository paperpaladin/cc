CFLAGS = -std=c11 -g -O1 -Wall -Wextra -Wno-unused-parameter -fno-common -ffile-prefix-map=$(CURDIR)=.

cc: cc.c
	$(CC) $(CFLAGS) -o $@ $<

test: cc
	@./t/run.sh
	@./t/cpp.sh

# Random programs, judged by gcc and clang. Not in `test` because it is
# slow and because its value is in volume: run it with a wide seed range.
# t/gen.py's header explains the rules that keep a generated program free of
# undefined behaviour, which is the entire difficulty.
fuzz: cc
	@./t/fuzz.sh $(FROM) $(TO)
FROM = 1
TO = 200

# The fixpoint, and the proof that the stage-2 compiler is more than stable.
# Not folded into `test`: it builds the compiler three times and runs the
# whole suite twice, which is minutes rather than seconds.
# 220 small C programs written by other people. Not vendored: the corpus
# gives no licence for the individual cases, so it is fetched, pinned to
# one commit and checked by sha256. Skips loudly when offline.
conformance: cc
	@t/cts.sh
self-host: cc
	@./t/selfhost.sh

# The same suite, run against a sanitized compiler. A compiler reads
# untrusted bytes for a living, and every test here already drives it over a
# program; the only thing missing was something watching the memory while it
# did. ASAN_OPTIONS=exitcode=86 is load-bearing: a sanitizer abort would
# otherwise exit 1, which is also exactly what a correct refusal looks like,
# and t/run.sh would score a crash as a pass.
#
# detect_leaks=0 is a decision, not a convenience. This compiler frees
# nothing on purpose -- see the Design section of the README -- so every run
# ends holding its whole AST, and LeakSanitizer is correct every time it
# complains. Leaving it on fails EVERY case -- measured: `detect_leaks=1`
# gives "0 passed, 325 failed" -- because every run ends holding its AST, so
# the number is just the case count. (This said 236 and the README said 237;
# both were absolutes for a figure that tracks the suite's size, which is why
# they drifted apart and away from the truth.) All of them saying the same
# known thing is the fastest way to stop reading the output. What is
# being asked here is whether the compiler reads or writes memory it does not
# own, and that question is still fully answered.
test-asan: cc.c
	$(CC) -std=c11 -g -O1 -fsanitize=address,undefined \
	    -fno-sanitize-recover=all -fno-omit-frame-pointer \
	    -Wall -Wextra -Wno-unused-parameter -o cc-asan cc.c
	@ASAN_OPTIONS=exitcode=86:detect_leaks=0 UBSAN_OPTIONS=exitcode=86 CCBIN=./cc-asan ./t/run.sh
	@ASAN_OPTIONS=exitcode=86:detect_leaks=0 UBSAN_OPTIONS=exitcode=86 CCBIN=./cc-asan ./t/cpp.sh

# Differential test: every t/ case must agree with gcc AND clang.
diff-test: cc
	@./t/differential.sh

# -rf, not -f: t/tmp*.promo and t/tmp*.shift are directories, left behind
# whenever a run.sh is interrupted, and `rm -f` on a directory fails -- so
# `make clean` exited 2 in any tree that had one.
clean:
	rm -rf cc cc-asan *.s *.cc.s a.out t/tmp* t/fuzz-fail-*.c

.PHONY: conformance test test-asan self-host fuzz diff-test clean
