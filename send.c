#include "librsswitch.h"

int main(int argc, char *argv[])
{
	if (argc < 5) {
		printf("Usage: %s DEV GROUP SOCKET DATA\n", argv[0]);
		exit(EXIT_FAILURE);
	}
	/*FIXME: Check user input */
	socket_send(argv[1], argv[2], argv[3], argv[4]);
	return 0;
}
