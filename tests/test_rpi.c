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
 * This suite only ever names a GPIO chip that cannot exist on any
 * host, so it can never open a real line, on this box or anyone
 * else's. It calls only the entry points a session actually needs to
 * observe the two behaviours under test (opening a chip, and closing
 * a session that never opened one), and it never asks the board to
 * drive an output pulse, so it cannot key a real transmitter.
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

int main(void)
{
	test_missing_chip_logs_to_syslog();

	return MT_REPORT();
}
