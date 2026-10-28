/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stub.h"
#include "../gpiod/gpiod_config.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

/*
 * The chip names that make the stub fail: "fail" followed by the wire
 * word of the cause. failother is the unclassified failure.
 */
static const struct stub_inject {
	const char *name;
	enum rss_err cause;
} inject_table[] = {
	{ "failnochip", RSS_ERR_NOCHIP },
	{ "failbusy", RSS_ERR_BUSY },
	{ "faildenied", RSS_ERR_DENIED },
	{ "failbadline", RSS_ERR_BADLINE },
	{ "failio", RSS_ERR_IO },
	{ "failother", RSS_ERR_OTHER },
};

static struct stub_pulse recorder[STUB_RECORDER_MAX_PULSES];
static size_t recorder_count;
static int recorder_overflow;
static size_t init_count;
static size_t exit_count;
static size_t fail_at_index = SIZE_MAX;
static size_t open_sessions;
static size_t overlap_count;

/* Set by board_init() for failio, read by transmit(). */
static int inject_io;

/*
 * Returns the cause a config asks the stub to fail with, or
 * RSS_ERR_NONE. Only a table name directly followed by ':' counts.
 * This is a plain prefix match and does not use the shared chip:line
 * parser, which logs on malformed input, because the stub's
 * board_init() still accepts any string.
 */
static enum rss_err injected_cause(const char *config)
{
	size_t i;

	if (!config)
		return RSS_ERR_NONE;

	for (i = 0; i < ARRAY_SIZE(inject_table); i++) {
		size_t len = strlen(inject_table[i].name);

		if (strncmp(config, inject_table[i].name, len) == 0 &&
		    config[len] == ':')
			return inject_table[i].cause;
	}

	return RSS_ERR_NONE;
}

/*
 * A chip named "fail" plus a cause word (the six names above) makes
 * the stub fail where hardware would: in board_init() with that cause,
 * or for failio at the first transmit(). Every send fails while such a
 * config is set. Any other config behaves as before. A failed
 * board_init() opens no session, because the library calls no
 * board_exit() after it.
 */
int32_t board_init(const char *config)
{
	enum rss_err cause = injected_cause(config);

	init_count++;
	inject_io = cause == RSS_ERR_IO;
	if (cause != RSS_ERR_NONE && cause != RSS_ERR_IO) {
		sched_yield();
		return -(int32_t)cause;
	}

	if (open_sessions > 0)
		overlap_count++;
	open_sessions++;
	sched_yield();
	return 0;
}

/*
 * The host build has no GPIO, but it applies the libgpiod boards'
 * rules, so the example programs refuse the same configs on every
 * build. It counts nothing and touches no recorder state.
 */
int32_t board_config_check(const char *config)
{
	char chipname[MAX_CHIPNAME_LEN];
	unsigned int line_offset;

	if (!config)
		return 0;

	return gpiod_config_parse(config, chipname, sizeof(chipname),
				  &line_offset);
}

int32_t board_exit(void)
{
	if (open_sessions > 0)
		open_sessions--;
	exit_count++;
	return 0;
}

/*
 * Prints the codeword to stdout, so the tests and the example
 * programs can read what a send is about to transmit. A failio chip
 * still gets it printed: that failure only hits the first transmit().
 */
void board_codeword(const char *codeword)
{
	fprintf(stdout, "%s\n", codeword);
}

int32_t transmit(uint32_t nhigh, uint32_t nlow, uint32_t pulse_len)
{
	if (inject_io) {
		sched_yield();
		return -(int32_t)RSS_ERR_IO;
	}

	if (recorder_count == fail_at_index) {
		fail_at_index = SIZE_MAX;
		sched_yield();
		return -1;
	}

	if (recorder_count >= STUB_RECORDER_MAX_PULSES) {
		recorder_overflow = 1;
		sched_yield();
		return 0;
	}

	recorder[recorder_count].nhigh = nhigh;
	recorder[recorder_count].nlow = nlow;
	recorder[recorder_count].pulse_len = pulse_len;
	recorder_count++;
	sched_yield();
	return 0;
}

void stub_recorder_reset(void)
{
	recorder_count = 0;
	recorder_overflow = 0;
	init_count = 0;
	exit_count = 0;
	fail_at_index = SIZE_MAX;
	inject_io = 0;
	open_sessions = 0;
	overlap_count = 0;
}

size_t stub_recorder_count(void)
{
	return recorder_count;
}

const struct stub_pulse *stub_recorder_get(size_t index)
{
	if (index >= recorder_count)
		return NULL;

	return &recorder[index];
}

int stub_recorder_overflowed(void)
{
	return recorder_overflow;
}

size_t stub_board_init_count(void)
{
	return init_count;
}

size_t stub_board_exit_count(void)
{
	return exit_count;
}

void stub_recorder_fail_at(size_t index)
{
	fail_at_index = index;
}

size_t stub_board_overlap_count(void)
{
	return overlap_count;
}
