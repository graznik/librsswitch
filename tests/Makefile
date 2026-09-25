CFLAGS += -Werror -Wall -W -pedantic -DX86_64
INCLUDES = -I..

.PHONY: test clean

test: test_socket_send
	./test_socket_send

test_socket_send: test_socket_send.c minitest.h ../librsswitch.c ../librsswitch.h
	$(CC) $(CFLAGS) $(INCLUDES) -o test_socket_send test_socket_send.c ../librsswitch.c

clean:
	-@rm -f test_socket_send 2>/dev/null || true
