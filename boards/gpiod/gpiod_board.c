#include <errno.h>
#include <gpiod.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <syslog.h>
#include <time.h>

#include "gpiod_board.h"
#include "gpiod_config.h"

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

/*
 * Maps errno of a failed chip open, line lookup or line request to a
 * board return value. It is pure: the caller copies errno right after
 * the failing call, before anything that can change it.
 *
 *	ENOENT, ENODEV, ENXIO	the chip does not exist
 *	EACCES, EPERM		no permission to open the chip
 *	EBUSY			the line is in use by another program
 *	EINVAL			the line offset is not valid for the chip
 *
 * Everything else is -(int32_t)SOCKET_ERR_OTHER, on purpose, because a
 * wrong cause is worse than none. Two cases are left as other deliberately:
 * ENOTTY, which libgpiod 1.x sets for a device node that exists but is
 * not a GPIO chip, and any errno that two different situations share.
 */
static int32_t cause_from_errno(int e)
{
	switch (e) {
	case ENOENT:
	case ENODEV:
	case ENXIO:
		return -(int32_t)SOCKET_ERR_NOCHIP;
	case EACCES:
	case EPERM:
		return -(int32_t)SOCKET_ERR_DENIED;
	case EBUSY:
		return -(int32_t)SOCKET_ERR_BUSY;
	case EINVAL:
		return -(int32_t)SOCKET_ERR_BADLINE;
	default:
		return -(int32_t)SOCKET_ERR_OTHER;
	}
}

/*
 * The format of the config string is documented in gpiod_config.h and
 * applied by gpiod_config_parse(). This writes the board's defaults to
 * the outputs and returns 0 for NULL, which selects them. Otherwise it
 * returns what gpiod_config_parse() returns: 0 for a well-formed config,
 * -1 for a malformed one. board_init() then fails without opening a
 * chip, so a typo or a stale config never drives a line the operator
 * did not name. Both libgpiod API versions share it; only how the chip
 * is opened differs (see board_init() below).
 */
static int parse_config(const char *config, char *chipname_out,
			size_t chipname_out_len, unsigned int *line_out)
{
	snprintf(chipname_out, chipname_out_len, "%s", DEFAULT_CHIPNAME);
	*line_out = DEFAULT_LINE_OFFSET;

	if (!config)
		return 0;

	return gpiod_config_parse(config, chipname_out, chipname_out_len,
				  line_out);
}

/*
 * Answers the board contract's question whether board_init() could use
 * config. It applies the same rules as board_init() and opens nothing.
 */
int32_t board_config_check(const char *config)
{
	char chipname[MAX_CHIPNAME_LEN];
	unsigned int line_offset;

	return parse_config(config, chipname, sizeof(chipname), &line_offset);
}

/*
 * Set to 1 only between a successful high write and the following
 * successful low write -- the window in which the line may actually
 * be high. A failed high write leaves it 0, since the failed write
 * did not change the level. board_exit() uses it to drive the line
 * low before releasing it, so a transmit() that failed between its
 * two writes cannot leave the transmitter keyed. Shared by
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

	if (parse_config(config, chipname, sizeof(chipname), &line_offset) < 0)
		return -1;

	chip = gpiod_chip_open_by_name(chipname);
	if (!chip) {
		int e = errno;

		syslog(LOG_ERR, "[-] Failed to open GPIO chip %s: %s",
		       chipname, strerror(e));
		return cause_from_errno(e);
	}

	line = gpiod_chip_get_line(chip, line_offset);
	if (!line) {
		int e = errno;

		syslog(LOG_ERR, "[-] Failed to get GPIO line %u: %s",
		       line_offset, strerror(e));
		gpiod_chip_close(chip);
		chip = NULL;
		return cause_from_errno(e);
	}

	if (gpiod_line_request_output(line, "librsswitch", 0) < 0) {
		int e = errno;

		syslog(LOG_ERR,
		       "[-] Failed to request GPIO line %u as output: %s",
		       line_offset, strerror(e));
		gpiod_chip_close(chip);
		chip = NULL;
		line = NULL;
		return cause_from_errno(e);
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
	 * the session never ends with the transmitter keyed.
	 * Best effort: a failure is logged and returned, but never
	 * skips the release/close below.
	 */
	if (line_high) {
		if (gpiod_line_set_value(line, 0) < 0) {
			int e = errno;

			syslog(LOG_ERR,
			       "[-] Failed to drive GPIO line low: %s",
			       strerror(e));
			ret = -(int32_t)SOCKET_ERR_IO;
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

/*
 * Room for "/dev/", the longest chip name gpiod_config_parse() accepts
 * (MAX_CHIPNAME_LEN - 1 characters) and the terminating NUL, so the
 * path built in board_init() can never be truncated.
 */
#define MAX_CHIPPATH_LEN (MAX_CHIPNAME_LEN + 5)

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
	int32_t ret = -1;

	if (parse_config(config, chipname, sizeof(chipname), &line_offset) < 0)
		return -1;

	snprintf(chippath, sizeof(chippath), "/dev/%s", chipname);

	chip = gpiod_chip_open(chippath);
	if (!chip) {
		int e = errno;

		syslog(LOG_ERR, "[-] Failed to open GPIO chip %s: %s",
		       chippath, strerror(e));
		return cause_from_errno(e);
	}

	/*
	 * The allocation and config-object failures below say nothing
	 * about the line, so they stay -1 ("other"): ENOMEM is not a
	 * cause the owner can act on, and add-line-settings sets EINVAL
	 * for bad arguments, which must not read as an invalid line.
	 */
	settings = gpiod_line_settings_new();
	if (!settings) {
		int e = errno;

		syslog(LOG_ERR,
		       "[-] Failed to allocate GPIO line settings: %s",
		       strerror(e));
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
		int e = errno;

		syslog(LOG_ERR, "[-] Failed to allocate GPIO line config: %s",
		       strerror(e));
		goto out;
	}
	if (gpiod_line_config_add_line_settings(line_cfg, &line_offset, 1,
						 settings) < 0) {
		int e = errno;

		syslog(LOG_ERR,
		       "[-] Failed to add GPIO line %u settings: %s",
		       line_offset, strerror(e));
		gpiod_line_config_free(line_cfg);
		goto out;
	}

	req_cfg = gpiod_request_config_new();
	if (!req_cfg) {
		int e = errno;

		syslog(LOG_ERR,
		       "[-] Failed to allocate GPIO request config: %s",
		       strerror(e));
		gpiod_line_config_free(line_cfg);
		goto out;
	}
	gpiod_request_config_set_consumer(req_cfg, "librsswitch");

	request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
	if (!request) {
		int e = errno;

		syslog(LOG_ERR,
		       "[-] Failed to request GPIO line %u as output: %s",
		       line_offset, strerror(e));
		ret = cause_from_errno(e);
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
	 * the session never ends with the transmitter keyed.
	 * Best effort: a failure is logged and returned, but never
	 * skips the release/close below.
	 */
	if (line_high) {
		if (gpiod_line_request_set_value(request, line_offset,
						  GPIOD_LINE_VALUE_INACTIVE) < 0) {
			int e = errno;

			syslog(LOG_ERR,
			       "[-] Failed to drive GPIO line low: %s",
			       strerror(e));
			ret = -(int32_t)SOCKET_ERR_IO;
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
		int e = errno;

		syslog(LOG_ERR, "[-] gpiod_line_set_value: %s", strerror(e));
		return -(int32_t)SOCKET_ERR_IO;
	}
	line_high = 1;
	delayMicroseconds(pulse_len * nhigh);
	if (gpiod_line_set_value(line, 0) < 0) {
		int e = errno;

		syslog(LOG_ERR, "[-] gpiod_line_set_value: %s", strerror(e));
		return -(int32_t)SOCKET_ERR_IO;
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
		int e = errno;

		syslog(LOG_ERR, "[-] gpiod_line_request_set_value: %s",
		       strerror(e));
		return -(int32_t)SOCKET_ERR_IO;
	}
	line_high = 1;
	delayMicroseconds(pulse_len * nhigh);
	if (gpiod_line_request_set_value(request, line_offset,
					  GPIOD_LINE_VALUE_INACTIVE) < 0) {
		int e = errno;

		syslog(LOG_ERR, "[-] gpiod_line_request_set_value: %s",
		       strerror(e));
		return -(int32_t)SOCKET_ERR_IO;
	}
	line_high = 0;
	delayMicroseconds(pulse_len * nlow);

	return 0;
}

#endif /* LIBRSSWITCH_GPIOD_VERSION */
