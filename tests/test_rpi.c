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

/*
 * Drives boards/rpi/rpi.c's error paths and config parser on any Linux
 * host, without a Raspberry Pi or a real GPIO chip. syslog is captured
 * the same way tests/test_logging.c and tests/test_bbb.c capture it --
 * #undef/#define syslog before including the source -- so error
 * messages can be asserted on directly. Included, not linked, so this
 * file can also reach parse_config(), board_init(), board_exit() and
 * their static state.
 *
 * test_missing_chip_logs_to_syslog(), test_exit_without_open_chip()
 * and test_parse_config() only ever name a GPIO chip that cannot
 * exist on any host, so they can never open a real line. The
 * transmit()/board_exit() tests below run only against dummy chip and
 * line/request handles pointing at test-local storage (never a real
 * chip): every libgpiod call rpi.c makes on a line -- set-value,
 * release/request-release, and chip-close -- is intercepted at link
 * time (-Wl,--wrap in tests/Makefile) and, for a dummy handle, only
 * records the requested level instead of touching real hardware. A
 * non-dummy handle is forwarded to the real libgpiod function, so this
 * suite still never opens a real chip or drives a real line, and
 * cannot key a real transmitter.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#undef syslog
#define syslog capture_syslog

static void capture_syslog(int priority, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

#include "minitest.h"
#include "../boards/rpi/rpi.c"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#define CAPTURE_MAX_MESSAGES 64
#define CAPTURE_MSG_LEN 256

struct captured_message {
	int priority;
	char text[CAPTURE_MSG_LEN];
};

static struct captured_message captured[CAPTURE_MAX_MESSAGES];
static size_t captured_n;

static void capture_syslog(int priority, const char *fmt, ...)
{
	va_list ap;

	if (captured_n >= CAPTURE_MAX_MESSAGES)
		return;

	va_start(ap, fmt);
	vsnprintf(captured[captured_n].text, sizeof(captured[captured_n].text),
		  fmt, ap);
	va_end(ap);

	captured[captured_n].priority = priority;
	captured_n++;
}

static void captured_reset(void)
{
	captured_n = 0;
}

static size_t captured_count(void)
{
	return captured_n;
}

/* Returns 1 if any captured message contains needle, 0 otherwise. */
static int captured_find(const char *needle)
{
	size_t i;

	for (i = 0; i < captured_n; i++) {
		if (strstr(captured[i].text, needle) != NULL)
			return 1;
	}

	return 0;
}

/*
 * Mock state for the G-01-5 transmit()/board_exit() tests below.
 * dummy_chip_obj and dummy_line_obj/dummy_request_obj are ordinary
 * file-scope objects whose addresses stand in for opaque libgpiod
 * handles: rpi.c and its wrapped libgpiod calls only ever compare
 * these pointers or pass them back to another wrapped call, never
 * dereference their contents. mock_session_open() points chip and
 * line/request at them, exactly as a real board_init() would leave
 * them after a successful session.
 */
static int dummy_chip_obj;
#if LIBRSSWITCH_GPIOD_VERSION == 1
static int dummy_line_obj;
#else
static int dummy_request_obj;
#endif

static int mock_level;
static int mock_set_calls;
static int mock_fail_low_writes;
static int mock_level_at_release;
static int mock_release_count;
static int mock_close_count;
#if LIBRSSWITCH_GPIOD_VERSION == 2
static unsigned int mock_last_offset;
#endif

#if LIBRSSWITCH_GPIOD_VERSION == 1

extern int __real_gpiod_line_set_value(struct gpiod_line *line, int value);
int __wrap_gpiod_line_set_value(struct gpiod_line *line, int value);
int __wrap_gpiod_line_set_value(struct gpiod_line *line, int value)
{
	if (line != (struct gpiod_line *)&dummy_line_obj)
		return __real_gpiod_line_set_value(line, value);

	if (value == 0 && mock_fail_low_writes > 0) {
		mock_fail_low_writes--;
		errno = EIO;
		return -1;
	}

	mock_level = value;
	mock_set_calls++;
	return 0;
}

extern void __real_gpiod_line_release(struct gpiod_line *line);
void __wrap_gpiod_line_release(struct gpiod_line *line);
void __wrap_gpiod_line_release(struct gpiod_line *line)
{
	if (line != (struct gpiod_line *)&dummy_line_obj) {
		__real_gpiod_line_release(line);
		return;
	}

	mock_level_at_release = mock_level;
	mock_release_count++;
}

#else /* LIBRSSWITCH_GPIOD_VERSION == 2 */

extern int __real_gpiod_line_request_set_value(struct gpiod_line_request *request,
						unsigned int offset,
						enum gpiod_line_value value);
int __wrap_gpiod_line_request_set_value(struct gpiod_line_request *request,
					 unsigned int offset,
					 enum gpiod_line_value value);
int __wrap_gpiod_line_request_set_value(struct gpiod_line_request *request,
					 unsigned int offset,
					 enum gpiod_line_value value)
{
	if (request != (struct gpiod_line_request *)&dummy_request_obj)
		return __real_gpiod_line_request_set_value(request, offset,
							    value);

	mock_last_offset = offset;

	if (value == GPIOD_LINE_VALUE_INACTIVE && mock_fail_low_writes > 0) {
		mock_fail_low_writes--;
		errno = EIO;
		return -1;
	}

	mock_level = (value == GPIOD_LINE_VALUE_ACTIVE) ? 1 : 0;
	mock_set_calls++;
	return 0;
}

extern void __real_gpiod_line_request_release(struct gpiod_line_request *request);
void __wrap_gpiod_line_request_release(struct gpiod_line_request *request);
void __wrap_gpiod_line_request_release(struct gpiod_line_request *request)
{
	if (request != (struct gpiod_line_request *)&dummy_request_obj) {
		__real_gpiod_line_request_release(request);
		return;
	}

	mock_level_at_release = mock_level;
	mock_release_count++;
}

#endif /* LIBRSSWITCH_GPIOD_VERSION */

extern void __real_gpiod_chip_close(struct gpiod_chip *chip);
void __wrap_gpiod_chip_close(struct gpiod_chip *chip);
void __wrap_gpiod_chip_close(struct gpiod_chip *chip)
{
	if (chip != (struct gpiod_chip *)&dummy_chip_obj) {
		__real_gpiod_chip_close(chip);
		return;
	}

	mock_close_count++;
}

static void mock_reset(void)
{
	mock_level = -1;
	mock_set_calls = 0;
	mock_fail_low_writes = 0;
	mock_level_at_release = -1;
	mock_release_count = 0;
	mock_close_count = 0;
#if LIBRSSWITCH_GPIOD_VERSION == 2
	mock_last_offset = 0;
#endif
}

/*
 * Points chip and line (v1) / request (v2) at the dummy handles, sets
 * v2's line_offset to DEFAULT_LINE_OFFSET, line_high to 0 and the
 * mock level to 0 -- exactly as board_init() leaves them after a
 * successful session (a freshly requested output line starts low).
 */
static void mock_session_open(void)
{
	mock_reset();

	chip = (struct gpiod_chip *)&dummy_chip_obj;
#if LIBRSSWITCH_GPIOD_VERSION == 1
	line = (struct gpiod_line *)&dummy_line_obj;
#else
	request = (struct gpiod_line_request *)&dummy_request_obj;
	line_offset = DEFAULT_LINE_OFFSET;
#endif
	line_high = 0;
	mock_level = 0;
}

/*
 * Redirects fd 2 (stderr) to a fresh temporary file for the duration
 * of a board_init() call, then restores it, and reports whether
 * anything was written. Used to prove the board no longer uses the
 * stdio error printer (perror()), which writes to stderr, now that
 * every error path logs through syslog instead.
 */
static off_t call_board_init_capturing_stderr(const char *config,
					       int32_t *ret_out)
{
	int saved_fd;
	int tmp_fd;
	char tmp_path[] = "/tmp/librsswitch-test-rpi-stderr-XXXXXX";
	off_t size;

	fflush(stderr);

	tmp_fd = mkstemp(tmp_path);
	if (tmp_fd == -1) {
		fprintf(stdout, "mkstemp failed: %s\n", strerror(errno));
		*ret_out = -1;
		return -1;
	}
	(void)unlink(tmp_path);

	saved_fd = dup(2);
	if (saved_fd == -1 || dup2(tmp_fd, 2) == -1) {
		fprintf(stdout, "dup/dup2 failed: %s\n", strerror(errno));
		close(tmp_fd);
		if (saved_fd != -1)
			close(saved_fd);
		*ret_out = -1;
		return -1;
	}

	*ret_out = board_init(config);

	fflush(stderr);
	dup2(saved_fd, 2);
	close(saved_fd);

	size = lseek(tmp_fd, 0, SEEK_END);
	close(tmp_fd);

	return size;
}

/*
 * A chip name that cannot exist fails board_init(), the failure is
 * reported only through syslog with a "[-]" prefix, and nothing is
 * written to stderr.
 */
static void test_missing_chip_logs_to_syslog(void)
{
	int32_t ret;
	off_t stderr_bytes;

	captured_reset();
	stderr_bytes = call_board_init_capturing_stderr(
		"librsswitch-test-nonexistent:17", &ret);

	CHECK_EQ(ret, -1);
	CHECK_EQ(stderr_bytes, 0);
	CHECK(captured_count() > 0);
	CHECK(captured_find("[-]"));
}

/*
 * board_exit() is safe to call after a failed board_init(), and safe
 * to call a second time -- neither call crashes, and both return 0.
 */
static void test_exit_without_open_chip(void)
{
	int32_t ret;

	captured_reset();
	(void)call_board_init_capturing_stderr(
		"librsswitch-test-nonexistent:17", &ret);
	CHECK_EQ(ret, -1);

	CHECK_EQ(board_exit(), 0);
	CHECK_EQ(board_exit(), 0);
}

/*
 * A malformed config falls all the way back to the compiled-in
 * default (gpiochip0:17), and one "[-] Malformed GPIO config" message
 * is logged.
 */
static void check_parse_config_default(const char *config)
{
	char chipname[MAX_CHIPNAME_LEN];
	unsigned int line_offset;

	captured_reset();
	parse_config(config, chipname, sizeof(chipname), &line_offset);

	CHECK(strcmp(chipname, DEFAULT_CHIPNAME) == 0);
	CHECK_EQ(line_offset, (unsigned int)DEFAULT_LINE_OFFSET);
	CHECK(captured_find("[-] Malformed GPIO config"));
}

static void test_parse_config(void)
{
	char chipname[MAX_CHIPNAME_LEN];
	unsigned int line_offset;
	static const char * const malformed[] = {
		"gpiochip3:17x", "gpiochip3:", "gpiochip3:-1",
		"gpiochip3:+5", "gpiochip3: 5", "gpiochip3:4294967296",
		"gpiochip3:99999999999999999999", "nocolon", ":17",
	};
	size_t i;

	/* NULL config: default, no log. */
	captured_reset();
	parse_config(NULL, chipname, sizeof(chipname), &line_offset);
	CHECK(strcmp(chipname, DEFAULT_CHIPNAME) == 0);
	CHECK_EQ(line_offset, (unsigned int)DEFAULT_LINE_OFFSET);

	/* Well-formed configs: chip and line taken from the string. */
	captured_reset();
	parse_config("gpiochip3:5", chipname, sizeof(chipname), &line_offset);
	CHECK(strcmp(chipname, "gpiochip3") == 0);
	CHECK_EQ(line_offset, 5U);

	captured_reset();
	parse_config("gpiochip3:007", chipname, sizeof(chipname),
		     &line_offset);
	CHECK(strcmp(chipname, "gpiochip3") == 0);
	CHECK_EQ(line_offset, 7U);

	/* Every malformed form falls back to gpiochip0:17, logged. */
	for (i = 0; i < ARRAY_SIZE(malformed); i++)
		check_parse_config_default(malformed[i]);
}

/*
 * transmit()'s low write fails once (armed via mock_fail_low_writes),
 * leaving the mocked line at 1 and transmit() returning -1.
 * board_exit() then drives it low itself before releasing (G-01-5):
 * level at release 0, release and chip close each called once, and
 * chip (and line/request) cleared to NULL.
 */
static void test_failed_low_write_driven_low_at_release(void)
{
	int32_t transmit_ret;
	int32_t exit_ret;

	mock_session_open();
	mock_fail_low_writes = 1;

	transmit_ret = transmit(1, 1, 1);
	CHECK_EQ(transmit_ret, -1);
	CHECK_EQ(mock_level, 1);

	captured_reset();
	exit_ret = board_exit();

	CHECK_EQ(exit_ret, 0);
	CHECK_EQ(mock_level_at_release, 0);
	CHECK_EQ(mock_release_count, 1);
	CHECK_EQ(mock_close_count, 1);
	CHECK(chip == NULL);
#if LIBRSSWITCH_GPIOD_VERSION == 1
	CHECK(line == NULL);
#else
	CHECK(request == NULL);
	CHECK_EQ(mock_last_offset, (unsigned int)DEFAULT_LINE_OFFSET);
#endif
}

/*
 * A session with no write failure makes 2 set-value calls in
 * transmit() and adds none in board_exit(): line_high is already 0
 * when board_exit() runs, so its drive-low step never fires.
 */
static void test_clean_session_adds_no_write(void)
{
	int32_t transmit_ret;
	int32_t exit_ret;

	mock_session_open();

	transmit_ret = transmit(1, 1, 1);
	CHECK_EQ(transmit_ret, 0);
	CHECK_EQ(mock_set_calls, 2);

	exit_ret = board_exit();
	CHECK_EQ(exit_ret, 0);
	CHECK_EQ(mock_set_calls, 2);
	CHECK_EQ(mock_level_at_release, 0);
}

/*
 * Two low-write failures are armed: transmit()'s own low write
 * consumes the first and returns -1 with the line left high;
 * board_exit()'s drive-low attempt consumes the second, also fails,
 * is logged, and makes board_exit() return -1 -- but release and chip
 * close still each run once, chip is still cleared to NULL, and the
 * line is left high (level at release 1). A second board_exit() call
 * (chip already NULL) returns 0 without touching the mock again.
 */
static void test_failed_drive_low_still_releases(void)
{
	int32_t transmit_ret;
	int32_t exit_ret;

	mock_session_open();
	mock_fail_low_writes = 2;

	transmit_ret = transmit(1, 1, 1);
	CHECK_EQ(transmit_ret, -1);

	captured_reset();
	exit_ret = board_exit();

	CHECK_EQ(exit_ret, -1);
	CHECK(captured_find("[-] Failed to drive GPIO line low"));
	CHECK_EQ(mock_release_count, 1);
	CHECK_EQ(mock_close_count, 1);
	CHECK(chip == NULL);
	CHECK_EQ(mock_level_at_release, 1);

	CHECK_EQ(board_exit(), 0);
}

int main(void)
{
	test_missing_chip_logs_to_syslog();
	test_exit_without_open_chip();
	test_parse_config();
	test_failed_low_write_driven_low_at_release();
	test_clean_session_adds_no_write();
	test_failed_drive_low_still_releases();

	return MT_REPORT();
}
