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

# A buffer larger than its data turns the sanitizer off, and this target is
# the only thing in the repository that can tell. Two things left slack in
# the buffer holding a source file. read_file allocates the file's size plus
# two, for a newline phase 1 may have to append and for a NUL, and usually
# only one of the two is used. And splice_lines DELETES a byte per line
# continuation without touching the allocation, which is the half no
# allocation size can see; glibc's headers use continuations constantly.
# Either way a read one byte past the end of the file landed inside a live
# allocation, AddressSanitizer had nothing to say, and test-asan went on
# printing a clean run over an area smaller than it looked.
#
# new_file now trims the text to its length, and every buffer that is ever
# tokenized goes through it. With that realloc reverted this target goes
# red while every other number the suite prints is unchanged -- measured,
# not assumed: t/run.sh still said 401 passed, 0 failed.
#
# -DCC_OVERREAD compiles a deliberate read of f->text[f->len + 1]. Exit 86
# because the compiler cannot produce it any other way: 0 is success, 1 is
# a clean refusal, 2 is a usage error, 127 is a failed exec and a crash
# arrives as a signal. Confusing a sanitizer report with a refusal is how a
# whole sanitizer build stops meaning anything, and test-asan above sets
# the same code for the same reason.
#
# The probe file carries a line continuation on purpose. Without one only
# read_file's two spare bytes are in play; the continuation is what makes
# the second, invisible half of the shape part of what is being gated.
overread:
	$(CC) -std=c11 -g -O1 -fsanitize=address -fno-omit-frame-pointer \
	    -Wall -Wextra -Wno-unused-parameter -DCC_OVERREAD -o cc-overread cc.c
	@printf 'int ma\\\nin(void) { return 0; }\n' > t/overread.c
	@ASAN_OPTIONS=exitcode=86:detect_leaks=0 \
	    ./cc-overread t/overread.c -o t/overread.bin > t/overread.log 2>&1; \
	 st=$$?; \
	 rm -f t/overread.c t/overread.bin; \
	 if [ "$$st" = 86 ] && grep -q heap-buffer-overflow t/overread.log; then \
	   echo "ok   the planted read past the file buffer is reported"; \
	   grep -m1 'READ of size' t/overread.log | sed 's/^/     /'; \
	   grep -m1 'in new_file' t/overread.log | sed 's/^/     /'; \
	   rm -f t/overread.log; \
	 else \
	   echo "FAIL a read one byte past the file buffer went unreported (exit $$st)"; \
	   echo "     The buffer is larger than its data again and the sanitizer"; \
	   echo "     cannot see into the slack. See new_file() in cc.c."; \
	   sed 's/^/     /' t/overread.log; \
	   rm -f t/overread.log; \
	   exit 1; \
	 fi

# Differential test: every t/ case must agree with gcc AND clang.
diff-test: cc
	@./t/differential.sh

# -rf, not -f: t/tmp*.promo and t/tmp*.shift are directories, left behind
# whenever a run.sh is interrupted, and `rm -f` on a directory fails -- so
# `make clean` exited 2 in any tree that had one.
clean:
	rm -rf cc cc-asan cc-overread t/overread.* *.s *.cc.s a.out t/tmp* t/fuzz-fail-*.c

.PHONY: conformance test test-asan overread self-host fuzz diff-test clean
