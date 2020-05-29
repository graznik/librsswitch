#include <stdlib.h>
#include <stdio.h>

#include "../../librsswitch.h"

int main(int argc, char *argv[])
{
	int ret;

	if (argc < 5) {
		printf("Usage: %s DEV GROUP SOCKET DATA\n", argv[0]);
		exit(EXIT_FAILURE);
	}
	/* FIXME: Check user input */
	ret = socket_send((uint32_t)atoi(argv[1]), (uint32_t)atoi(argv[2]),
			  (uint32_t)atoi(argv[3]), (uint32_t)atoi(argv[4]));

	return ret;
}
