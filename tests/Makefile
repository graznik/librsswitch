CFLAGS += -Werror -Wall -W -pedantic -DX86_64 -pthread
INCLUDES = -I..

TESTS = test_socket_send test_codewords test_concurrency test_codeword_api \
	test_logging test_bbb
LIB_SRCS = ../librsswitch.c ../boards/x86_64/x86_64.c
TEST_DEPS = minitest.h pulse_decode.h ../librsswitch.c ../librsswitch.h \
	    ../boards/board.h ../boards/x86_64/x86_64.c ../boards/x86_64/x86_64.h

.PHONY: test clean tsan

test: $(TESTS)
	@status=0; \
	for t in $(TESTS); do \
		./$$t > /dev/null || status=1; \
	done; \
	exit $$status

test_socket_send: test_socket_send.c $(TEST_DEPS)
	$(CC) $(CFLAGS) $(INCLUDES) -o test_socket_send test_socket_send.c $(LIB_SRCS)

test_codewords: test_codewords.c $(TEST_DEPS)
	$(CC) $(CFLAGS) $(INCLUDES) -o test_codewords test_codewords.c $(LIB_SRCS)

test_concurrency: test_concurrency.c $(TEST_DEPS)
	$(CC) $(CFLAGS) $(INCLUDES) -o test_concurrency test_concurrency.c $(LIB_SRCS)

test_codeword_api: test_codeword_api.c $(TEST_DEPS)
	$(CC) $(CFLAGS) $(INCLUDES) -o test_codeword_api test_codeword_api.c ../boards/x86_64/x86_64.c

# Built with -DDEBUG so the DEBUG-only "dev: ..., group: ..., socket:
# ..., data: ..." line in socket_send() is exercised (LIB-02).
# Includes ../librsswitch.c directly (like test_codeword_api), so it
# links only the x86_64 board, not librsswitch.c a second time.
test_logging: test_logging.c $(TEST_DEPS)
	$(CC) $(CFLAGS) -DDEBUG $(INCLUDES) -o test_logging test_logging.c ../boards/x86_64/x86_64.c

# Includes ../boards/bbb/bbb.c directly (like test_logging includes
# ../librsswitch.c), so it can drive board_init()/transmit()/
# board_exit() and their static state against a fake sysfs tree
# (the override is defined in test_bbb.c). Links no other source.
test_bbb: test_bbb.c minitest.h ../boards/bbb/bbb.c ../boards/bbb/bbb.h \
	  ../boards/board.h
	$(CC) $(CFLAGS) $(INCLUDES) -o test_bbb test_bbb.c

# Optional ThreadSanitizer target (D-10). `make tsan` builds
# test_concurrency with -fsanitize=thread -g -O1 into
# test_concurrency_tsan (the name matches the tests/test_* ignore
# pattern) and runs it. It is opt-in and never a prerequisite of
# `test`, since not every host ships libtsan. On kernels with high
# mmap randomization, a GCC ThreadSanitizer binary can abort with
# "unexpected memory mapping" when run directly, so it runs under
# `setarch "$(uname -m)" -R` when setarch exists, and directly
# otherwise.
tsan: test_concurrency_tsan
	@if command -v setarch > /dev/null 2>&1; then \
		setarch "$$(uname -m)" -R ./test_concurrency_tsan > /dev/null; \
	else \
		./test_concurrency_tsan > /dev/null; \
	fi

test_concurrency_tsan: test_concurrency.c $(TEST_DEPS)
	$(CC) $(CFLAGS) -fsanitize=thread -g -O1 $(INCLUDES) -o test_concurrency_tsan test_concurrency.c $(LIB_SRCS)

clean:
	-@rm -f $(TESTS) test_concurrency_tsan 2>/dev/null || true
	-@rm -rf test_bbb_sysfs /tmp/librsswitch-test-bbb-sysfs 2>/dev/null || true
