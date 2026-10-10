CFLAGS = -std=c11 -g -O1 -Wall -Wextra -Wno-unused-parameter -fno-common -ffile-prefix-map=$(CURDIR)=.

cc: cc.c
	$(CC) $(CFLAGS) -o $@ $<

test: cc
	@./t/run.sh
	@./t/cpp.sh

# The fixpoint, and the proof that the stage-2 compiler is more than stable.
# Not folded into `test`: it builds the compiler three times and runs the
# whole suite twice, which is minutes rather than seconds.
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
# complains. Leaving it on would mean 236 failures that all say the same
# known thing, which is the fastest way to stop reading the output. What is
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

clean:
	rm -f cc cc-asan *.s *.cc.s a.out t/tmp*

.PHONY: test test-asan self-host diff-test clean
