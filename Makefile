CFLAGS = -std=c11 -g -O1 -Wall -Wextra -Wno-unused-parameter -fno-common

cc: cc.c
	$(CC) $(CFLAGS) -o $@ $<

test: cc
	@./t/run.sh

# Differential test: every t/ case must agree with gcc AND clang.
diff-test: cc
	@./t/differential.sh

clean:
	rm -f cc *.s *.cc.s a.out t/tmp*

.PHONY: test diff-test clean
