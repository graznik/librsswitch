#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "../../librsswitch.h"

int main(int argc, char *argv[])
{
	int ret;
	const char *gpio_config = NULL;

	if (argc < 5) {
		printf("Usage: %s DEV GROUP SOCKET DATA [--gpio CONFIG]\n",
		       argv[0]);
		exit(EXIT_FAILURE);
	}

	if (argc >= 7 && strcmp(argv[5], "--gpio") == 0)
		gpio_config = argv[6];

	/* FIXME: Check user input */
	socket_gpio_config(gpio_config);
	ret = socket_send((uint32_t)atoi(argv[1]), (uint32_t)atoi(argv[2]),
			  (uint32_t)atoi(argv[3]), (uint32_t)atoi(argv[4]));

	return ret;
}
