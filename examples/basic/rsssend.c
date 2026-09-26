#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

/*
 * Parses arg as a base-10 uint32_t, rejecting everything strtoul()
 * would otherwise silently accept: a leading sign, whitespace, a hex
 * prefix, trailing garbage, an empty string and overflow past
 * UINT32_MAX (Pitfall 4). On success writes the value to *out and
 * returns 0; on failure prints "rsssend: invalid NAME 'arg'" to
 * stderr and returns -1.
 */
static int parse_u32(const char *name, const char *arg, uint32_t *out)
{
	unsigned long val;
	char *end;

	if (arg[0] == '\0' || strspn(arg, "0123456789") != strlen(arg))
		goto invalid;

	errno = 0;
	val = strtoul(arg, &end, 10);
	if (errno == ERANGE || val > UINT32_MAX || *end != '\0')
		goto invalid;

	*out = (uint32_t)val;
	return 0;

invalid:
	fprintf(stderr, "rsssend: invalid %s '%s'\n", name, arg);
	return -1;
}

int main(int argc, char *argv[])
{
	const char *gpio_config = NULL;
	uint32_t dev, group, isocket, data;
	int ret;

	if (argc == 5) {
		gpio_config = NULL;
	} else if (argc == 7 && strcmp(argv[5], "--gpio") == 0) {
		gpio_config = argv[6];
	} else {
		usage();
		exit(2);
	}

	if (parse_u32("DEV", argv[1], &dev) < 0)
		exit(2);
	if (dev != REV_008345 && dev != POLLIN_2605) {
		fprintf(stderr, "rsssend: invalid DEV '%s'\n", argv[1]);
		exit(2);
	}

	if (parse_u32("GROUP", argv[2], &group) < 0)
		exit(2);

	if (parse_u32("SOCKET", argv[3], &isocket) < 0)
		exit(2);

	if (parse_u32("DATA", argv[4], &data) < 0)
		exit(2);
	if (data != 0 && data != 1) {
		fprintf(stderr, "rsssend: invalid DATA '%s'\n", argv[4]);
		exit(2);
	}

	socket_gpio_config(gpio_config);
	ret = socket_send(dev, group, isocket, data);
	if (ret < 0) {
		fprintf(stderr,
			"rsssend: sending DEV %" PRIu32 " GROUP %" PRIu32
			" SOCKET %" PRIu32 " DATA %" PRIu32
			" failed (see syslog)\n",
			dev, group, isocket, data);
		exit(1);
	}

	return 0;
}
