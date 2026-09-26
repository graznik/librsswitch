CFLAGS += -Werror -Wall -W -pedantic -DX86_64
INCLUDES = -I..

TESTS = test_socket_send test_codewords
LIB_SRCS = ../librsswitch.c ../boards/x86_64/x86_64.c
TEST_DEPS = minitest.h pulse_decode.h ../librsswitch.c ../librsswitch.h \
	    ../boards/board.h ../boards/x86_64/x86_64.c ../boards/x86_64/x86_64.h

.PHONY: test clean

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

clean:
	-@rm -f $(TESTS) 2>/dev/null || true
