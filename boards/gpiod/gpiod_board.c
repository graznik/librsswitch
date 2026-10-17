#include <errno.h>
#include <gpiod.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <syslog.h>
#include <time.h>

#include "gpiod_board.h"

/*
 * Board code for every board that drives its line through libgpiod.
 * Each such board supplies only its default chip and line, in
 * boards/<board>/<board>.h; gpiod_board.h picks them by board macro.
 */

/*
 * libgpiod dropped the line-handle API (gpiod_line_*) for a
 * request-based one in 2.x, so the board picks its API at compile
 * time. GPIOD_LINE_BULK_MAX_LINES is a macro only the 1.x header
 * defines, so its presence means 1.x is installed; its absence means
 * the 2.x header is. LIBRSSWITCH_GPIOD_VERSION can be set on the
 * command line (see the LIBGPIOD_VERSION Makefile variable) to
 * override the probe if it ever picks the wrong one.
 */
#ifndef LIBRSSWITCH_GPIOD_VERSION
#ifdef GPIOD_LINE_BULK_MAX_LINES
#define LIBRSSWITCH_GPIOD_VERSION 1
#else
#define LIBRSSWITCH_GPIOD_VERSION 2
#endif
#endif

#if LIBRSSWITCH_GPIOD_VERSION != 1 && LIBRSSWITCH_GPIOD_VERSION != 2
#error "LIBRSSWITCH_GPIOD_VERSION must be 1 or 2"
#endif

#define MAX_CHIPNAME_LEN 64

/*
 * config format: "<chip>:<line>", e.g. "gpiochip0:17". NULL, or a
 * config whose chip or line part cannot be parsed, falls back to the
 * board's DEFAULT_CHIPNAME and DEFAULT_LINE_OFFSET from
 * gpiod_board.h (gpiochip0:17 on every board today). The line
 * part is accepted only as a non-empty string of the digits 0-9, with
 * no sign, no surrounding whitespace and no trailing garbage --
 * anything strtoul() would otherwise parse leniently (Pitfall 4) is
 * rejected instead of silently misread. The format is the same for
 * both libgpiod API versions; only how the chip is opened differs
 * (see board_init() below).
 */
static void parse_config(const char *config, char *chipname_out,
			 size_t chipname_out_len, unsigned int *line_out)
{
	const char *sep;
	const char *linestr;
	size_t chiplen;
	size_t i;
	unsigned long parsed_line;
	char *end;

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

	linestr = sep + 1;
	if (linestr[0] == '\0')
		goto bad_line;
	for (i = 0; linestr[i] != '\0'; i++) {
		if (linestr[i] < '0' || linestr[i] > '9')
			goto bad_line;
	}

	errno = 0;
	parsed_line = strtoul(linestr, &end, 10);
	if (errno == ERANGE)
		goto bad_line;
	if (parsed_line > UINT_MAX)
		goto bad_line;
	if (*end != '\0')
		goto bad_line;

	memcpy(chipname_out, config, chiplen);
	chipname_out[chiplen] = '\0';
	*line_out = (unsigned int)parsed_line;
	return;

bad_line:
	syslog(LOG_ERR,
	       "[-] Malformed GPIO config '%s' (bad line number), using default",
	       config);
}

/*
 * Set to 1 only between a successful high write and the following
 * successful low write -- the window in which the line may actually
 * be high. A failed high write leaves it 0, since the failed write
 * did not change the level. board_exit() uses it to drive the line
 * low before releasing it, so a transmit() that failed between its
 * two writes cannot leave the transmitter keyed (G-01-5). Shared by
 * both libgpiod API variants below.
 */
static int line_high;

#if LIBRSSWITCH_GPIOD_VERSION == 1

static struct gpiod_chip *chip;
static struct gpiod_line *line;

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
		chip = NULL;
		return -1;
	}

	if (gpiod_line_request_output(line, "librsswitch", 0) < 0) {
		syslog(LOG_ERR,
		       "[-] Failed to request GPIO line %u as output: %s",
		       line_offset, strerror(errno));
		gpiod_chip_close(chip);
		chip = NULL;
		line = NULL;
		return -1;
	}
	/* The line was just requested as a low output. */
	line_high = 0;

	return 0;
}

int32_t board_exit(void)
{
	int32_t ret = 0;

	if (!chip)
		return 0;

	/*
	 * A transmit() that failed between its high and low writes
	 * leaves the line high; drive it low here, before release, so
	 * the session never ends with the transmitter keyed (G-01-5).
	 * Best effort: a failure is logged and returned, but never
	 * skips the release/close below.
	 */
	if (line_high) {
		if (gpiod_line_set_value(line, 0) < 0) {
			syslog(LOG_ERR,
			       "[-] Failed to drive GPIO line low: %s",
			       strerror(errno));
			ret = -1;
		}
		line_high = 0;
	}

	gpiod_line_release(line);
	gpiod_chip_close(chip);
	chip = NULL;
	line = NULL;

	return ret;
}

#else /* LIBRSSWITCH_GPIOD_VERSION == 2 */

#define MAX_CHIPPATH_LEN (MAX_CHIPNAME_LEN + 5) /* "/dev/" + chipname */

static struct gpiod_chip *chip;
static struct gpiod_line_request *request;
static unsigned int line_offset;

/*
 * v2 opens chips by path ("/dev/<chipname>"), not by the bare name
 * v1 used. Requesting the line as an output takes three short-lived
 * config objects (settings, line config, request config) that are
 * freed again once gpiod_chip_request_lines() has consumed them --
 * on every path, success or failure.
 */
int32_t board_init(const char *config)
{
	char chipname[MAX_CHIPNAME_LEN];
	char chippath[MAX_CHIPPATH_LEN];
	struct gpiod_line_settings *settings;
	struct gpiod_line_config *line_cfg;
	struct gpiod_request_config *req_cfg;
	int n;
	int32_t ret = -1;

	parse_config(config, chipname, sizeof(chipname), &line_offset);

	n = snprintf(chippath, sizeof(chippath), "/dev/%s", chipname);
	if (n < 0 || (size_t)n >= sizeof(chippath)) {
		syslog(LOG_ERR, "[-] GPIO chip name too long: %s", chipname);
		return -1;
	}

	chip = gpiod_chip_open(chippath);
	if (!chip) {
		syslog(LOG_ERR, "[-] Failed to open GPIO chip %s: %s",
		       chippath, strerror(errno));
		return -1;
	}

	settings = gpiod_line_settings_new();
	if (!settings) {
		syslog(LOG_ERR,
		       "[-] Failed to allocate GPIO line settings: %s",
		       strerror(errno));
		gpiod_chip_close(chip);
		chip = NULL;
		return -1;
	}
	gpiod_line_settings_set_direction(settings,
					   GPIOD_LINE_DIRECTION_OUTPUT);
	gpiod_line_settings_set_output_value(settings,
					      GPIOD_LINE_VALUE_INACTIVE);

	line_cfg = gpiod_line_config_new();
	if (!line_cfg) {
		syslog(LOG_ERR, "[-] Failed to allocate GPIO line config: %s",
		       strerror(errno));
		goto out;
	}
	if (gpiod_line_config_add_line_settings(line_cfg, &line_offset, 1,
						 settings) < 0) {
		syslog(LOG_ERR,
		       "[-] Failed to add GPIO line %u settings: %s",
		       line_offset, strerror(errno));
		goto out;
	}

	req_cfg = gpiod_request_config_new();
	if (!req_cfg) {
		syslog(LOG_ERR,
		       "[-] Failed to allocate GPIO request config: %s",
		       strerror(errno));
		gpiod_line_config_free(line_cfg);
		goto out;
	}
	gpiod_request_config_set_consumer(req_cfg, "librsswitch");

	request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
	if (!request) {
		syslog(LOG_ERR,
		       "[-] Failed to request GPIO line %u as output: %s",
		       line_offset, strerror(errno));
	} else {
		ret = 0;
		/* The line was just requested as a low output. */
		line_high = 0;
	}

	gpiod_request_config_free(req_cfg);
	gpiod_line_config_free(line_cfg);

out:
	gpiod_line_settings_free(settings);

	if (ret < 0) {
		gpiod_chip_close(chip);
		chip = NULL;
	}

	return ret;
}

int32_t board_exit(void)
{
	int32_t ret = 0;

	if (!chip)
		return 0;

	/*
	 * A transmit() that failed between its high and low writes
	 * leaves the line high; drive it low here, before release, so
	 * the session never ends with the transmitter keyed (G-01-5).
	 * Best effort: a failure is logged and returned, but never
	 * skips the release/close below.
	 */
	if (line_high) {
		if (gpiod_line_request_set_value(request, line_offset,
						  GPIOD_LINE_VALUE_INACTIVE) < 0) {
			syslog(LOG_ERR,
			       "[-] Failed to drive GPIO line low: %s",
			       strerror(errno));
			ret = -1;
		}
		line_high = 0;
	}

	gpiod_line_request_release(request);
	gpiod_chip_close(chip);
	chip = NULL;
	request = NULL;

	return ret;
}

#endif /* LIBRSSWITCH_GPIOD_VERSION */

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

#if LIBRSSWITCH_GPIOD_VERSION == 1

int32_t transmit(uint32_t nhigh, uint32_t nlow, uint32_t pulse_len)
{
	if (gpiod_line_set_value(line, 1) < 0) {
		syslog(LOG_ERR, "[-] gpiod_line_set_value");
		return -1;
	}
	line_high = 1;
	delayMicroseconds(pulse_len * nhigh);
	if (gpiod_line_set_value(line, 0) < 0) {
		syslog(LOG_ERR, "[-] gpiod_line_set_value");
		return -1;
	}
	line_high = 0;
	delayMicroseconds(pulse_len * nlow);

	return 0;
}

#else /* LIBRSSWITCH_GPIOD_VERSION == 2 */

int32_t transmit(uint32_t nhigh, uint32_t nlow, uint32_t pulse_len)
{
	if (gpiod_line_request_set_value(request, line_offset,
					  GPIOD_LINE_VALUE_ACTIVE) < 0) {
		syslog(LOG_ERR, "[-] gpiod_line_request_set_value");
		return -1;
	}
	line_high = 1;
	delayMicroseconds(pulse_len * nhigh);
	if (gpiod_line_request_set_value(request, line_offset,
					  GPIOD_LINE_VALUE_INACTIVE) < 0) {
		syslog(LOG_ERR, "[-] gpiod_line_request_set_value");
		return -1;
	}
	line_high = 0;
	delayMicroseconds(pulse_len * nlow);

	return 0;
}

#endif /* LIBRSSWITCH_GPIOD_VERSION */
