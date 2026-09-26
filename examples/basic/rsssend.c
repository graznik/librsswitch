#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "../../librsswitch.h"

static void usage(void)
{
	fprintf(stderr,
		"Usage: rsssend DEV GROUP SOCKET DATA [--gpio CONFIG]\n"
		"  DEV:  0 = REV 008345, 1 = Pollin 2605\n"
		"  DATA: 0 = off, 1 = on\n"
		"Exit status: 0 sent, 1 send failed, 2 usage error\n");
}

int main(int argc, char *argv[])
{
	int ret;
	const char *gpio_config = NULL;

	if (argc < 5) {
		usage();
		exit(2);
	}

	if (argc >= 7 && strcmp(argv[5], "--gpio") == 0)
		gpio_config = argv[6];

	/* FIXME: Check user input */
	socket_gpio_config(gpio_config);
	ret = socket_send((uint32_t)atoi(argv[1]), (uint32_t)atoi(argv[2]),
			  (uint32_t)atoi(argv[3]), (uint32_t)atoi(argv[4]));

	if (ret < 0) {
		fprintf(stderr,
			"rsssend: sending DEV %s GROUP %s SOCKET %s DATA %s"
			" failed (see syslog)\n",
			argv[1], argv[2], argv[3], argv[4]);
		exit(1);
	}

	return 0;
}
