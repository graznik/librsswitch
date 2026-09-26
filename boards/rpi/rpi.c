#include <errno.h>
#include <gpiod.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <syslog.h>

#include "rpi.h"

#define DEFAULT_CHIPNAME "gpiochip0"
#define DEFAULT_LINE_OFFSET 17
#define MAX_CHIPNAME_LEN 64

static struct gpiod_chip *chip;
static struct gpiod_line *line;

/*
 * config format: "<chip>:<line>", e.g. "gpiochip0:17". NULL or an
 * unparseable string falls back to gpiochip0:17 (this board's
 * previous hardcoded behaviour).
 */
static void parse_config(const char *config, char *chipname_out,
			 size_t chipname_out_len, unsigned int *line_out)
{
	const char *sep;
	size_t chiplen;

	snprintf(chipname_out, chipname_out_len, "%s", DEFAULT_CHIPNAME);
	*line_out = DEFAULT_LINE_OFFSET;

	if (!config)
		return;

	sep = strchr(config, ':');
	if (!sep) {
		syslog(LOG_ERR,
		       "[-] Malformed GPIO config '%s' (expected chip:line), using default",
		       config);
		return;
	}

	chiplen = (size_t)(sep - config);
	if (chiplen == 0 || chiplen >= chipname_out_len) {
		syslog(LOG_ERR,
		       "[-] Malformed GPIO config '%s' (bad chip name), using default",
		       config);
		return;
	}

	memcpy(chipname_out, config, chiplen);
	chipname_out[chiplen] = '\0';
	*line_out = (unsigned int)strtoul(sep + 1, NULL, 10);
}

int32_t board_init(const char *config)
{
	char chipname[MAX_CHIPNAME_LEN];
	unsigned int line_offset;

	parse_config(config, chipname, sizeof(chipname), &line_offset);

	chip = gpiod_chip_open_by_name(chipname);
	if (!chip) {
		syslog(LOG_ERR, "[-] Failed to open GPIO chip %s: %s",
		       chipname, strerror(errno));
		return -1;
	}

	line = gpiod_chip_get_line(chip, line_offset);
	if (!line) {
		syslog(LOG_ERR, "[-] Failed to get GPIO line %u: %s",
		       line_offset, strerror(errno));
		gpiod_chip_close(chip);
		return -1;
	}

	if (gpiod_line_request_output(line, "librsswitch", 0) < 0) {
		syslog(LOG_ERR,
		       "[-] Failed to request GPIO line %u as output: %s",
		       line_offset, strerror(errno));
		gpiod_chip_close(chip);
		return -1;
	}

	return 0;
}

int32_t board_exit(void)
{
	gpiod_chip_close(chip);
	return 0;
}

void delayMicrosecondsHard(unsigned int howLong)
{
	struct timeval tNow, tLong, tEnd;

	gettimeofday(&tNow, NULL);
	tLong.tv_sec  = howLong / 1000000;
	tLong.tv_usec = howLong % 1000000;
	timeradd(&tNow, &tLong, &tEnd);

	while (timercmp(&tNow, &tEnd, <))
		gettimeofday(&tNow, NULL);
}

void delayMicroseconds(unsigned int howLong)
{
	struct timespec sleeper;
	unsigned int uSecs = howLong % 1000000;
	unsigned int wSecs = howLong / 1000000;

	if (howLong == 0)
		return;
	else if (howLong < 100)
		delayMicrosecondsHard(howLong);
	else {
		sleeper.tv_sec  = wSecs;
		sleeper.tv_nsec = (long)(uSecs * 1000L);
		nanosleep(&sleeper, NULL);
	}
}

int32_t transmit(uint32_t nhigh, uint32_t nlow, uint32_t pulse_len)
{
	if (gpiod_line_set_value(line, 1) < 0) {
		syslog(LOG_ERR, "[-] gpiod_line_set_value");
		return -1;
	}
	delayMicroseconds(pulse_len * nhigh);
	if (gpiod_line_set_value(line, 0) < 0) {
		syslog(LOG_ERR, "[-] gpiod_line_set_value");
		return -1;
	}
	delayMicroseconds(pulse_len * nlow);

	return 0;
}
