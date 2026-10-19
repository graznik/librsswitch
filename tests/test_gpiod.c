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
 * Drives boards/gpiod/gpiod_board.c's error paths and, through
 * boards/gpiod/gpiod_config.c, its config parser on any Linux host,
 * without a Raspberry Pi or a real GPIO chip. syslog is captured the
 * same way tests/test_logging.c captures it -- #undef/#define syslog
 * before including the sources -- so error messages can be asserted on
 * directly. Both files are included, not linked, so this file can also
 * reach parse_config(), board_init(), board_exit() and their static
 * state.
 *
 * The file is built once per libgpiod board, as test_gpiod_rpi and
 * test_gpiod_bbb, so each build also checks that board's defaults.
 *
 * test_missing_chip_logs_to_syslog(), test_exit_without_open_chip()
 * and test_parse_config() only ever name a GPIO chip that cannot
 * exist on any host, so they can never open a real line. The
 * transmit()/board_exit() tests below run only against dummy chip and
 * line/request handles pointing at test-local storage (never a real
 * chip): every libgpiod call gpiod_board.c makes on a line --
 * set-value, release/request-release, and chip-close -- is intercepted
 * at link time (-Wl,--wrap in tests/Makefile) and, for a dummy handle,
 * only records the requested level instead of touching real hardware.
 * A non-dummy handle is forwarded to the real libgpiod function, so
 * this suite still never opens a real chip or drives a real line, and
 * cannot key a real transmitter. The board_init() tests open only the
 * dummy chip through the open wraps, so they never open a real chip
 * either.
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
#include "../boards/gpiod/gpiod_config.c"
#include "../boards/gpiod/gpiod_board.c"

/* tests/Makefile sets EXPECT_GPIOD_VERSION from GPIOD_EXPECT_VERSION. */
#if defined(EXPECT_GPIOD_VERSION) && \
	EXPECT_GPIOD_VERSION != LIBRSSWITCH_GPIOD_VERSION
#error "built against a libgpiod major version other than GPIOD_EXPECT_VERSION"
#endif

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
 * Mock state for the transmit()/board_exit() tests below.
 * dummy_chip_obj and dummy_line_obj/dummy_request_obj are ordinary
 * file-scope objects whose addresses stand in for opaque libgpiod
 * handles: gpiod_board.c and its wrapped libgpiod calls only ever
 * compare these pointers or pass them back to another wrapped call,
 * never dereference their contents. mock_session_open() points chip
 * and line/request at them, exactly as a real board_init() would leave
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

/*
 * Mock state for the board_init() tests. board_init() runs against the
 * same dummy handles: the chip-open wraps below return the dummy chip
 * only while mock_fake_chip_open is set and forward to the real libgpiod
 * otherwise, so the tests that need a real failed open still get one.
 * Every later call on the dummy chip is answered by a mock. On libgpiod
 * 2.x the config objects board_init() builds stay real; their
 * allocations and frees are counted, so a test can see that none is
 * left behind.
 */
static int mock_fake_chip_open;
static int mock_open_calls;
static char mock_open_arg[80];
static int mock_consumer_ok;
#if LIBRSSWITCH_GPIOD_VERSION == 1
static int mock_fail_get_line;
static int mock_fail_request_output;
static unsigned int mock_get_line_offset;
static int mock_request_default;
#else
static int mock_fail_settings_new;
static int mock_fail_line_cfg_new;
static int mock_fail_req_cfg_new;
static int mock_fail_add_settings;
static int mock_fail_request_lines;
static int mock_settings_live;
static int mock_line_cfg_live;
static int mock_req_cfg_live;
static unsigned int mock_request_offset;
static size_t mock_request_num_offsets;
#endif

#if LIBRSSWITCH_GPIOD_VERSION == 1

extern struct gpiod_chip *__real_gpiod_chip_open_by_name(const char *name);
struct gpiod_chip *__wrap_gpiod_chip_open_by_name(const char *name);
struct gpiod_chip *__wrap_gpiod_chip_open_by_name(const char *name)
{
	mock_open_calls++;
	snprintf(mock_open_arg, sizeof(mock_open_arg), "%s", name);

	if (!mock_fake_chip_open)
		return __real_gpiod_chip_open_by_name(name);

	return (struct gpiod_chip *)&dummy_chip_obj;
}

extern struct gpiod_line *__real_gpiod_chip_get_line(struct gpiod_chip *chip,
						      unsigned int offset);
struct gpiod_line *__wrap_gpiod_chip_get_line(struct gpiod_chip *chip,
					       unsigned int offset);
struct gpiod_line *__wrap_gpiod_chip_get_line(struct gpiod_chip *chip,
					       unsigned int offset)
{
	if (chip != (struct gpiod_chip *)&dummy_chip_obj)
		return __real_gpiod_chip_get_line(chip, offset);

	mock_get_line_offset = offset;

	if (mock_fail_get_line) {
		errno = EINVAL;
		return NULL;
	}

	return (struct gpiod_line *)&dummy_line_obj;
}

extern int __real_gpiod_line_request_output(struct gpiod_line *line,
					     const char *consumer,
					     int default_val);
int __wrap_gpiod_line_request_output(struct gpiod_line *line,
				      const char *consumer, int default_val);
int __wrap_gpiod_line_request_output(struct gpiod_line *line,
				      const char *consumer, int default_val)
{
	if (line != (struct gpiod_line *)&dummy_line_obj)
		return __real_gpiod_line_request_output(line, consumer,
							 default_val);

	mock_request_default = default_val;
	if (consumer != NULL && strcmp(consumer, "librsswitch") == 0)
		mock_consumer_ok = 1;

	if (mock_fail_request_output) {
		errno = EBUSY;
		return -1;
	}

	mock_level = default_val;
	return 0;
}

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

extern struct gpiod_chip *__real_gpiod_chip_open(const char *path);
struct gpiod_chip *__wrap_gpiod_chip_open(const char *path);
struct gpiod_chip *__wrap_gpiod_chip_open(const char *path)
{
	mock_open_calls++;
	snprintf(mock_open_arg, sizeof(mock_open_arg), "%s", path);

	if (!mock_fake_chip_open)
		return __real_gpiod_chip_open(path);

	return (struct gpiod_chip *)&dummy_chip_obj;
}

extern struct gpiod_line_settings *__real_gpiod_line_settings_new(void);
struct gpiod_line_settings *__wrap_gpiod_line_settings_new(void);
struct gpiod_line_settings *__wrap_gpiod_line_settings_new(void)
{
	struct gpiod_line_settings *settings;

	if (mock_fail_settings_new) {
		errno = ENOMEM;
		return NULL;
	}

	settings = __real_gpiod_line_settings_new();
	if (settings != NULL)
		mock_settings_live++;

	return settings;
}

extern void __real_gpiod_line_settings_free(struct gpiod_line_settings *settings);
void __wrap_gpiod_line_settings_free(struct gpiod_line_settings *settings);
void __wrap_gpiod_line_settings_free(struct gpiod_line_settings *settings)
{
	if (settings != NULL)
		mock_settings_live--;

	__real_gpiod_line_settings_free(settings);
}

extern struct gpiod_line_config *__real_gpiod_line_config_new(void);
struct gpiod_line_config *__wrap_gpiod_line_config_new(void);
struct gpiod_line_config *__wrap_gpiod_line_config_new(void)
{
	struct gpiod_line_config *config;

	if (mock_fail_line_cfg_new) {
		errno = ENOMEM;
		return NULL;
	}

	config = __real_gpiod_line_config_new();
	if (config != NULL)
		mock_line_cfg_live++;

	return config;
}

extern void __real_gpiod_line_config_free(struct gpiod_line_config *config);
void __wrap_gpiod_line_config_free(struct gpiod_line_config *config);
void __wrap_gpiod_line_config_free(struct gpiod_line_config *config)
{
	if (config != NULL)
		mock_line_cfg_live--;

	__real_gpiod_line_config_free(config);
}

extern int __real_gpiod_line_config_add_line_settings(
	struct gpiod_line_config *config, const unsigned int *offsets,
	size_t num_offsets, struct gpiod_line_settings *settings);
int __wrap_gpiod_line_config_add_line_settings(
	struct gpiod_line_config *config, const unsigned int *offsets,
	size_t num_offsets, struct gpiod_line_settings *settings);
int __wrap_gpiod_line_config_add_line_settings(
	struct gpiod_line_config *config, const unsigned int *offsets,
	size_t num_offsets, struct gpiod_line_settings *settings)
{
	if (mock_fail_add_settings) {
		errno = ENOMEM;
		return -1;
	}

	return __real_gpiod_line_config_add_line_settings(config, offsets,
							  num_offsets,
							  settings);
}

extern struct gpiod_request_config *__real_gpiod_request_config_new(void);
struct gpiod_request_config *__wrap_gpiod_request_config_new(void);
struct gpiod_request_config *__wrap_gpiod_request_config_new(void)
{
	struct gpiod_request_config *config;

	if (mock_fail_req_cfg_new) {
		errno = ENOMEM;
		return NULL;
	}

	config = __real_gpiod_request_config_new();
	if (config != NULL)
		mock_req_cfg_live++;

	return config;
}

extern void __real_gpiod_request_config_free(struct gpiod_request_config *config);
void __wrap_gpiod_request_config_free(struct gpiod_request_config *config);
void __wrap_gpiod_request_config_free(struct gpiod_request_config *config)
{
	if (config != NULL)
		mock_req_cfg_live--;

	__real_gpiod_request_config_free(config);
}

extern struct gpiod_line_request *__real_gpiod_chip_request_lines(
	struct gpiod_chip *chip, struct gpiod_request_config *req_cfg,
	struct gpiod_line_config *line_cfg);
struct gpiod_line_request *__wrap_gpiod_chip_request_lines(
	struct gpiod_chip *chip, struct gpiod_request_config *req_cfg,
	struct gpiod_line_config *line_cfg);
struct gpiod_line_request *__wrap_gpiod_chip_request_lines(
	struct gpiod_chip *chip, struct gpiod_request_config *req_cfg,
	struct gpiod_line_config *line_cfg)
{
	unsigned int offsets[1];
	const char *consumer;

	if (chip != (struct gpiod_chip *)&dummy_chip_obj)
		return __real_gpiod_chip_request_lines(chip, req_cfg, line_cfg);

	mock_request_num_offsets =
		gpiod_line_config_get_num_configured_offsets(line_cfg);
	if (mock_request_num_offsets == 1) {
		gpiod_line_config_get_configured_offsets(line_cfg, offsets,
							  ARRAY_SIZE(offsets));
		mock_request_offset = offsets[0];
	}

	consumer = gpiod_request_config_get_consumer(req_cfg);
	if (consumer != NULL && strcmp(consumer, "librsswitch") == 0)
		mock_consumer_ok = 1;

	if (mock_fail_request_lines) {
		errno = EBUSY;
		return NULL;
	}

	mock_level = 0;
	return (struct gpiod_line_request *)&dummy_request_obj;
}

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
	mock_fake_chip_open = 0;
	mock_open_calls = 0;
	mock_open_arg[0] = '\0';
	mock_consumer_ok = 0;
#if LIBRSSWITCH_GPIOD_VERSION == 1
	mock_fail_get_line = 0;
	mock_fail_request_output = 0;
	mock_get_line_offset = 0;
	mock_request_default = 0;
#else
	mock_fail_settings_new = 0;
	mock_fail_line_cfg_new = 0;
	mock_fail_req_cfg_new = 0;
	mock_fail_add_settings = 0;
	mock_fail_request_lines = 0;
	mock_settings_live = 0;
	mock_line_cfg_live = 0;
	mock_req_cfg_live = 0;
	mock_request_offset = 0;
	mock_request_num_offsets = 0;
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
	char tmp_path[] = "/tmp/librsswitch-test-gpiod-stderr-XXXXXX";
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

/* Every form of GPIO config the parser must refuse. */
static const char * const malformed[] = {
	"gpiochip3:17x", "gpiochip3:", "gpiochip3:-1",
	"gpiochip3:+5", "gpiochip3: 5", "gpiochip3:4294967296",
	"gpiochip3:99999999999999999999", "nocolon", ":17",
	/*
	 * A bare number was the BeagleBone Black's old GPIO
	 * config and is malformed now, like any other string
	 * without a chip name.
	 */
	"17", "49", "gpiochip0", "gpiochip0:17:5",
	/*
	 * A chip name is a bare device name, and a slash would
	 * let it point outside /dev.
	 */
	"gpiochip0/../x:3", "/dev/gpiochip0:17", "../tmp/x:3",
	/*
	 * A chip name starts with a letter or digit, so "." and ".."
	 * cannot name a directory, and neither can a hidden or an
	 * option-like name.
	 */
	"..:3", ".:3", ".hidden:3", "-x:3",
};

/*
 * A malformed config is refused: parse_config() returns -1 and logs
 * one "[-] Malformed GPIO config" message.
 */
static void check_parse_config_malformed(const char *config)
{
	char chipname[MAX_CHIPNAME_LEN];
	unsigned int line_offset;

	captured_reset();
	CHECK_EQ(parse_config(config, chipname, sizeof(chipname),
			      &line_offset), -1);

	CHECK_EQ(captured_count(), (size_t)1);
	CHECK(captured_find("[-] Malformed GPIO config"));
}

static void test_parse_config(void)
{
	char chipname[MAX_CHIPNAME_LEN];
	unsigned int line_offset;
	size_t i;

	/* NULL config: default, no log. */
	captured_reset();
	CHECK_EQ(parse_config(NULL, chipname, sizeof(chipname),
			      &line_offset), 0);
	CHECK(strcmp(chipname, DEFAULT_CHIPNAME) == 0);
	CHECK_EQ(line_offset, (unsigned int)DEFAULT_LINE_OFFSET);
	CHECK_EQ(captured_count(), (size_t)0);

	/* Well-formed configs: chip and line taken from the string. */
	captured_reset();
	CHECK_EQ(parse_config("gpiochip3:5", chipname, sizeof(chipname),
			      &line_offset), 0);
	CHECK(strcmp(chipname, "gpiochip3") == 0);
	CHECK_EQ(line_offset, 5U);
	CHECK_EQ(captured_count(), (size_t)0);

	captured_reset();
	CHECK_EQ(parse_config("gpiochip3:007", chipname, sizeof(chipname),
			      &line_offset), 0);
	CHECK(strcmp(chipname, "gpiochip3") == 0);
	CHECK_EQ(line_offset, 7U);
	CHECK_EQ(captured_count(), (size_t)0);

	/* Every malformed form is refused and logged once. */
	for (i = 0; i < ARRAY_SIZE(malformed); i++)
		check_parse_config_malformed(malformed[i]);
}

/*
 * board_config_check() answers like parse_config() without opening a
 * chip: 0 and silence for NULL and for a well-formed config, -1 and
 * one "[-] Malformed GPIO config" message for each malformed one.
 */
static void test_board_config_check(void)
{
	size_t i;

	mock_reset();

	captured_reset();
	CHECK_EQ(board_config_check(NULL), 0);
	CHECK_EQ(captured_count(), (size_t)0);

	captured_reset();
	CHECK_EQ(board_config_check("gpiochip3:5"), 0);
	CHECK_EQ(captured_count(), (size_t)0);

	for (i = 0; i < ARRAY_SIZE(malformed); i++) {
		captured_reset();
		CHECK_EQ(board_config_check(malformed[i]), -1);
		CHECK_EQ(captured_count(), (size_t)1);
		CHECK(captured_find("[-] Malformed GPIO config"));
	}

	CHECK_EQ(mock_open_calls, 0);
}

/*
 * Both libgpiod boards default to gpiochip0 line 17: the Raspberry Pi
 * to GPIO17 on the 40-pin header, the BeagleBone Black to P9_23 on
 * kernel 6.x. Each build must take its own board's values, and must
 * not see the other board's macros.
 */
static void test_board_defaults(void)
{
	CHECK(strcmp(DEFAULT_CHIPNAME, "gpiochip0") == 0);
	CHECK_EQ(DEFAULT_LINE_OFFSET, 17);

#if defined(RASPBERRY_PI)
	CHECK(strcmp(DEFAULT_CHIPNAME, RPI_DEFAULT_CHIPNAME) == 0);
	CHECK_EQ(DEFAULT_LINE_OFFSET, RPI_DEFAULT_LINE_OFFSET);
#ifdef BBB_DEFAULT_CHIPNAME
	CHECK(0);
#endif
#ifdef BBB_DEFAULT_LINE_OFFSET
	CHECK(0);
#endif
#elif defined(BEAGLEBONE_BLACK)
	CHECK(strcmp(DEFAULT_CHIPNAME, BBB_DEFAULT_CHIPNAME) == 0);
	CHECK_EQ(DEFAULT_LINE_OFFSET, BBB_DEFAULT_LINE_OFFSET);
#ifdef RPI_DEFAULT_CHIPNAME
	CHECK(0);
#endif
#ifdef RPI_DEFAULT_LINE_OFFSET
	CHECK(0);
#endif
#endif
}

/*
 * transmit()'s low write fails once (armed via mock_fail_low_writes),
 * leaving the mocked line at 1 and transmit() returning -1.
 * board_exit() then drives it low itself before releasing:
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

/*
 * A successful board_init() opens the configured chip once, requests
 * the configured line once as a low output for consumer "librsswitch",
 * logs nothing and leaves line_high at 0 even if it was 1 before.
 * board_exit() then releases the line and closes the chip once each,
 * and clears chip (and line/request).
 */
static void test_board_init_success(void)
{
	mock_reset();
	captured_reset();
	mock_fake_chip_open = 1;
	line_high = 1;

	CHECK_EQ(board_init("gpiochip9:3"), 0);

	CHECK_EQ(mock_open_calls, 1);
#if LIBRSSWITCH_GPIOD_VERSION == 1
	CHECK(strcmp(mock_open_arg, "gpiochip9") == 0);
	CHECK(line == (struct gpiod_line *)&dummy_line_obj);
	CHECK_EQ(mock_get_line_offset, 3U);
	CHECK_EQ(mock_request_default, 0);
#else
	CHECK(strcmp(mock_open_arg, "/dev/gpiochip9") == 0);
	CHECK(request == (struct gpiod_line_request *)&dummy_request_obj);
	CHECK_EQ(line_offset, 3U);
	CHECK_EQ(mock_request_num_offsets, (size_t)1);
	CHECK_EQ(mock_request_offset, 3U);
	CHECK_EQ(mock_settings_live, 0);
	CHECK_EQ(mock_line_cfg_live, 0);
	CHECK_EQ(mock_req_cfg_live, 0);
#endif
	CHECK(chip == (struct gpiod_chip *)&dummy_chip_obj);
	CHECK(mock_consumer_ok);
	CHECK_EQ(mock_level, 0);
	CHECK_EQ(line_high, 0);
	CHECK_EQ(captured_count(), (size_t)0);
	CHECK_EQ(mock_close_count, 0);

	CHECK_EQ(board_exit(), 0);
	CHECK_EQ(mock_set_calls, 0);
	CHECK_EQ(mock_release_count, 1);
	CHECK_EQ(mock_close_count, 1);
	CHECK(chip == NULL);
#if LIBRSSWITCH_GPIOD_VERSION == 1
	CHECK(line == NULL);
#else
	CHECK(request == NULL);
#endif

	mock_reset();
}

/*
 * After a failed board_init() the chip is closed exactly once and
 * cleared, so a following board_exit() succeeds and closes nothing
 * again.
 */
static void check_board_init_failed_cleanly(void)
{
	CHECK_EQ(mock_close_count, 1);
	CHECK(chip == NULL);
#if LIBRSSWITCH_GPIOD_VERSION == 2
	CHECK_EQ(mock_settings_live, 0);
	CHECK_EQ(mock_line_cfg_live, 0);
	CHECK_EQ(mock_req_cfg_live, 0);
#endif

	CHECK_EQ(board_exit(), 0);
	CHECK_EQ(mock_close_count, 1);
	CHECK_EQ(mock_release_count, 0);
}

/*
 * A malformed config makes board_init() fail before any GPIO chip is
 * opened: no open is even attempted, nothing is left open, and the
 * error is logged. The chip open is armed to hand out the dummy chip,
 * so an open attempt would be visible both in the call count and as a
 * successful return.
 */
static void test_board_init_malformed_opens_no_chip(void)
{
	static const char * const configs[] = {
		"49", "gpiochip0", "gpiochip0/../x:3",
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(configs); i++) {
		mock_reset();
		captured_reset();
		mock_fake_chip_open = 1;

		CHECK_EQ(board_init(configs[i]), -1);
		CHECK_EQ(mock_open_calls, 0);
		CHECK(chip == NULL);
		CHECK(captured_find("[-] Malformed GPIO config"));
		CHECK_EQ(board_exit(), 0);

		mock_reset();
	}
}

/*
 * A NULL config selects the board's default chip and line, without
 * logging anything.
 */
static void test_board_init_null_config_opens_default(void)
{
	mock_reset();
	captured_reset();
	mock_fake_chip_open = 1;

	CHECK_EQ(board_init(NULL), 0);

	CHECK_EQ(mock_open_calls, 1);
#if LIBRSSWITCH_GPIOD_VERSION == 1
	CHECK(strcmp(mock_open_arg, DEFAULT_CHIPNAME) == 0);
	CHECK_EQ(mock_get_line_offset, (unsigned int)DEFAULT_LINE_OFFSET);
#else
	CHECK(strcmp(mock_open_arg, "/dev/" DEFAULT_CHIPNAME) == 0);
	CHECK_EQ(mock_request_offset, (unsigned int)DEFAULT_LINE_OFFSET);
#endif
	CHECK_EQ(captured_count(), (size_t)0);

	CHECK_EQ(board_exit(), 0);

	mock_reset();
}

#if LIBRSSWITCH_GPIOD_VERSION == 1

/* gpiod_chip_get_line() fails: the chip is closed and the error logged. */
static void test_board_init_get_line_fails(void)
{
	mock_reset();
	captured_reset();
	mock_fake_chip_open = 1;
	mock_fail_get_line = 1;

	CHECK_EQ(board_init("gpiochip9:3"), -1);
	CHECK(captured_find("[-] Failed to get GPIO line 3"));
	check_board_init_failed_cleanly();

	mock_reset();
}

/*
 * gpiod_line_request_output() fails: the chip is closed, chip and line
 * are cleared and the error is logged.
 */
static void test_board_init_request_output_fails(void)
{
	mock_reset();
	captured_reset();
	mock_fake_chip_open = 1;
	mock_fail_request_output = 1;

	CHECK_EQ(board_init("gpiochip9:3"), -1);
	CHECK(captured_find("[-] Failed to request GPIO line 3 as output"));
	CHECK(line == NULL);
	check_board_init_failed_cleanly();

	mock_reset();
}

#else /* LIBRSSWITCH_GPIOD_VERSION == 2 */

/*
 * Each of the three config objects fails to allocate in turn: board_init()
 * returns -1, closes the chip once, frees whatever it had allocated up to
 * that point and logs which allocation failed.
 */
static void test_board_init_alloc_fails(void)
{
	int * const flags[] = {
		&mock_fail_settings_new, &mock_fail_line_cfg_new,
		&mock_fail_req_cfg_new,
	};
	static const char * const messages[] = {
		"[-] Failed to allocate GPIO line settings",
		"[-] Failed to allocate GPIO line config",
		"[-] Failed to allocate GPIO request config",
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(flags); i++) {
		mock_reset();
		captured_reset();
		mock_fake_chip_open = 1;
		*flags[i] = 1;

		CHECK_EQ(board_init("gpiochip9:3"), -1);
		CHECK(captured_find(messages[i]));
		check_board_init_failed_cleanly();

		mock_reset();
	}
}

/*
 * gpiod_chip_request_lines() fails: the chip is closed, all three
 * config objects are freed, request is cleared and the error is logged.
 */
static void test_board_init_request_lines_fails(void)
{
	mock_reset();
	captured_reset();
	mock_fake_chip_open = 1;
	mock_fail_request_lines = 1;

	CHECK_EQ(board_init("gpiochip9:3"), -1);
	CHECK(captured_find("[-] Failed to request GPIO line 3 as output"));
	CHECK(request == NULL);
	check_board_init_failed_cleanly();

	mock_reset();
}

/*
 * gpiod_line_config_add_line_settings() fails: board_init() returns -1,
 * closes the chip once, frees the line settings, the line config and
 * the request config and logs the error. libgpiod fails this call here
 * only when it runs out of memory, and the line config must still be
 * freed.
 */
static void test_board_init_frees_line_config_on_add_failure(void)
{
	mock_reset();
	captured_reset();
	mock_fake_chip_open = 1;
	mock_fail_add_settings = 1;

	CHECK_EQ(board_init("gpiochip9:3"), -1);
	CHECK(captured_find("[-] Failed to add GPIO line 3 settings"));
	CHECK(request == NULL);
	check_board_init_failed_cleanly();

	mock_reset();
}

#endif /* LIBRSSWITCH_GPIOD_VERSION */

int main(void)
{
	test_missing_chip_logs_to_syslog();
	test_exit_without_open_chip();
	test_parse_config();
	test_board_config_check();
	test_board_defaults();
	test_failed_low_write_driven_low_at_release();
	test_clean_session_adds_no_write();
	test_failed_drive_low_still_releases();
	test_board_init_success();
	test_board_init_malformed_opens_no_chip();
	test_board_init_null_config_opens_default();
#if LIBRSSWITCH_GPIOD_VERSION == 1
	test_board_init_get_line_fails();
	test_board_init_request_output_fails();
#else
	test_board_init_alloc_fails();
	test_board_init_frees_line_config_on_add_failure();
	test_board_init_request_lines_fails();
#endif

	return MT_REPORT();
}
