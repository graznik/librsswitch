#include "../../librsswitch.h"

int main(int argc, char *argv[])
{
	if (argc < 5) {
		printf("Usage: %s DEV GROUP SOCKET DATA\n", argv[0]);
		exit(EXIT_FAILURE);
	}
	/* FIXME: Check user input */
	socket_send(atoi(argv[1]), atoi(argv[2]),
		    atoi(argv[3]), atoi(argv[4]));
	return 0;
}
