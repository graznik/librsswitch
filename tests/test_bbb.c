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
 * Drives boards/bbb/bbb.c against a fake sysfs tree, so its sysfs
 * error handling can be exercised on any Linux host without a real
 * BeagleBone. BBB_SYSFS_GPIO_DIR is defined below, before including
 * the board source, redirecting every sysfs path this board touches
 * under a scratch directory. syslog is captured the same way
 * tests/test_logging.c captures it -- #undef/#define syslog before
 * including the source -- so error messages can be asserted on
 * directly. Included, not linked, like test_logging.c and
 * test_codeword_api.c, so this file can also reach board_init(),
 * board_exit() and transmit() and their static state.
 *
 * The fake tree lives under /tmp rather than relative to the
 * working directory: some of the failure scenarios below replace a
 * fake sysfs file with a symlink to /dev/full to inject a
 * deterministic write() failure, and a checkout of this repository
 * can be mounted on a filesystem (e.g. vboxsf, a VirtualBox shared
 * folder) that refuses to create symlinks at all ("Operation not
 * permitted"), while /tmp is a normal filesystem that supports them.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#define BBB_SYSFS_GPIO_DIR "/tmp/librsswitch-test-bbb-sysfs"

#undef syslog
#define syslog capture_syslog

static void capture_syslog(int priority, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

#include "minitest.h"
#include "../boards/bbb/bbb.c"

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

/* Creates or truncates path and writes content (no trailing NUL). */
static void fake_write_file(const char *path, const char *content)
{
	int fd;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd == -1) {
		fprintf(stderr, "fake_write_file: open %s: %s\n", path,
			strerror(errno));
		return;
	}

	if (write(fd, content, strlen(content)) < 0)
		fprintf(stderr, "fake_write_file: write %s: %s\n", path,
			strerror(errno));

	close(fd);
}

/*
 * Removes the fake tree's known entries, ignoring ENOENT (nothing to
 * remove is not an error here), then recreates the base directory
 * with empty export and unexport files.
 */
static void fake_reset(void)
{
	(void)unlink(BBB_SYSFS_GPIO_DIR "/gpio49/direction");
	(void)unlink(BBB_SYSFS_GPIO_DIR "/gpio49/value");
	(void)rmdir(BBB_SYSFS_GPIO_DIR "/gpio49");
	(void)unlink(BBB_SYSFS_GPIO_DIR "/export");
	(void)unlink(BBB_SYSFS_GPIO_DIR "/unexport");
	(void)rmdir(BBB_SYSFS_GPIO_DIR);

	if (mkdir(BBB_SYSFS_GPIO_DIR, 0755) == -1 && errno != EEXIST)
		fprintf(stderr, "fake_reset: mkdir %s: %s\n",
			BBB_SYSFS_GPIO_DIR, strerror(errno));

	fake_write_file(BBB_SYSFS_GPIO_DIR "/export", "");
	fake_write_file(BBB_SYSFS_GPIO_DIR "/unexport", "");

	captured_reset();
}

/* Creates gpio<pinstr>/ with empty direction and value files. */
static void fake_add_pin(const char *pinstr)
{
	char path[MAX_LEN];

	snprintf(path, sizeof(path), "%s/gpio%s", BBB_SYSFS_GPIO_DIR, pinstr);
	if (mkdir(path, 0755) == -1 && errno != EEXIST)
		fprintf(stderr, "fake_add_pin: mkdir %s: %s\n", path,
			strerror(errno));

	snprintf(path, sizeof(path), "%s/gpio%s/direction",
		 BBB_SYSFS_GPIO_DIR, pinstr);
	fake_write_file(path, "");

	snprintf(path, sizeof(path), "%s/gpio%s/value",
		 BBB_SYSFS_GPIO_DIR, pinstr);
	fake_write_file(path, "");
}

/*
 * Replaces the file at BBB_SYSFS_GPIO_DIR + relpath with a symlink
 * to /dev/full, so a write() through it deterministically fails
 * with ENOSPC instead of succeeding.
 */
static void fake_to_dev_full(const char *relpath)
{
	char path[MAX_LEN];

	snprintf(path, sizeof(path), "%s%s", BBB_SYSFS_GPIO_DIR, relpath);
	(void)unlink(path);
	if (symlink("/dev/full", path) == -1)
		fprintf(stderr, "fake_to_dev_full: symlink %s: %s\n", path,
			strerror(errno));
}

/* Returns the content of BBB_SYSFS_GPIO_DIR + relpath in buf, or
 * "<missing>" if the file does not exist.
 */
static void fake_read(const char *relpath, char *buf, size_t len)
{
	char path[MAX_LEN];
	int fd;
	ssize_t n;

	snprintf(path, sizeof(path), "%s%s", BBB_SYSFS_GPIO_DIR, relpath);
	fd = open(path, O_RDONLY);
	if (fd == -1) {
		snprintf(buf, len, "<missing>");
		return;
	}

	n = read(fd, buf, len - 1);
	if (n < 0)
		n = 0;
	buf[n] = '\0';

	close(fd);
}

/*
 * Counts this process's open file descriptors via /proc/self/fd, so
 * a test can assert a session leaves no fd behind (BRD-02).
 */
static long count_open_fds(void)
{
	DIR *d;
	struct dirent *de;
	long n = 0;

	d = opendir("/proc/self/fd");
	if (d == NULL) {
		fprintf(stderr, "count_open_fds: opendir: %s\n",
			strerror(errno));
		return -1;
	}

	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] != '.')
			n++;
	}

	closedir(d);

	return n;
}

static void fake_cleanup(void)
{
	(void)unlink(BBB_SYSFS_GPIO_DIR "/gpio49/direction");
	(void)unlink(BBB_SYSFS_GPIO_DIR "/gpio49/value");
	(void)rmdir(BBB_SYSFS_GPIO_DIR "/gpio49");
	(void)unlink(BBB_SYSFS_GPIO_DIR "/export");
	(void)unlink(BBB_SYSFS_GPIO_DIR "/unexport");
	(void)rmdir(BBB_SYSFS_GPIO_DIR);
}

/*
 * Baseline: with gpio49 pre-created (simulating a pin already
 * exported by udev or an administrator), board_init()/transmit()/
 * board_exit() run end to end, skipping the export write and the
 * later unexport -- export and unexport stay empty, since this
 * session never owned the export (D-17).
 */
static void test_lifecycle_with_fake_sysfs(void)
{
	char buf[MAX_LEN];

	fake_reset();
	fake_add_pin("49");

	CHECK_EQ(board_init(NULL), 0);
	fake_read("/export", buf, sizeof(buf));
	CHECK_EQ(strcmp(buf, ""), 0);
	fake_read("/gpio49/direction", buf, sizeof(buf));
	CHECK_EQ(strcmp(buf, "out"), 0);

	CHECK_EQ(transmit(1, 1, 1), 0);
	fake_read("/gpio49/value", buf, sizeof(buf));
	CHECK_EQ(strcmp(buf, "10"), 0);

	CHECK_EQ(board_exit(), 0);
	fake_read("/unexport", buf, sizeof(buf));
	CHECK_EQ(strcmp(buf, ""), 0);
}

/* A missing export file makes board_init() fail and log an error. */
static void test_missing_export_file(void)
{
	fake_reset();
	(void)unlink(BBB_SYSFS_GPIO_DIR "/export");

	CHECK_EQ(board_init(NULL), -1);
	CHECK(captured_count() > 0);
	CHECK_EQ(captured_find("this message was never logged"), 0);
}

/*
 * A value file replaced by a symlink to /dev/full makes transmit()
 * fail on its first write() (ENOSPC) instead of terminating the
 * process; the test keeps running afterwards. The symlink is put in
 * place before board_init(), which now opens the value fd for the
 * whole session (BRD-02) -- replacing the file afterwards would
 * leave the already-open fd pointing at the original (now unlinked)
 * file instead of at /dev/full.
 */
static void test_transmit_write_failure_returns(void)
{
	fake_reset();
	fake_add_pin("49");
	fake_to_dev_full("/gpio49/value");

	CHECK_EQ(board_init(NULL), 0);
	captured_reset();
	CHECK_EQ(transmit(1, 1, 1), -1);
	CHECK(captured_count() > 0);

	CHECK_EQ(board_exit(), 0);
}

/*
 * A missing value file makes board_init()'s open() fail, returning
 * -1 instead of terminating the process. Since board_init() now
 * opens the value fd for the whole session (BRD-02), a missing
 * value file is caught there rather than by a later transmit().
 */
static void test_transmit_missing_value_returns(void)
{
	fake_reset();
	fake_add_pin("49");
	(void)unlink(BBB_SYSFS_GPIO_DIR "/gpio49/value");

	captured_reset();
	CHECK_EQ(board_init(NULL), -1);
	CHECK(captured_count() > 0);
}

/*
 * An unexport file replaced by a symlink to /dev/full makes
 * board_exit()'s write() fail (ENOSPC), returning -1 instead of
 * terminating the process. board_exit() only attempts an unexport
 * when this session owns the export (pin_exported), which the fake
 * tree cannot arrange together with a fully successful session --
 * nothing creates gpio<pin> in response to writing export, so a
 * board_init() that performs its own export always then fails
 * opening direction (see the D-17 backstop truths in
 * 01-CONTEXT.md). pin_exported is set directly here (this file
 * includes boards/bbb/bbb.c, so its statics are reachable) to
 * exercise the unexport-write-failure path regardless.
 */
static void test_board_exit_unexport_failure_returns(void)
{
	fake_reset();
	fake_add_pin("49");
	CHECK_EQ(board_init(NULL), 0);
	pin_exported = 1;

	fake_to_dev_full("/unexport");
	captured_reset();
	CHECK_EQ(board_exit(), -1);
	CHECK(captured_count() > 0);

	pin_exported = 0;
}

/*
 * Every captured LOG_ERR message starts with "[-] ", names the path
 * it refers to, and contains no embedded newline (D-19).
 */
static void check_messages_follow_convention(void)
{
	size_t i;

	for (i = 0; i < captured_n; i++) {
		CHECK(strncmp(captured[i].text, "[-] ", 4) == 0);
		CHECK(strstr(captured[i].text, BBB_SYSFS_GPIO_DIR) != NULL);
		CHECK(strchr(captured[i].text, '\n') == NULL);
	}
}

/* Re-runs the Task 1 failure scenarios, checking every message
 * captured along the way against the D-19 logging convention.
 */
static void test_error_messages_follow_convention(void)
{
	fake_reset();
	(void)unlink(BBB_SYSFS_GPIO_DIR "/export");
	CHECK_EQ(board_init(NULL), -1);
	check_messages_follow_convention();

	fake_reset();
	fake_add_pin("49");
	fake_to_dev_full("/gpio49/value");
	CHECK_EQ(board_init(NULL), 0);
	captured_reset();
	CHECK_EQ(transmit(1, 1, 1), -1);
	check_messages_follow_convention();
	CHECK_EQ(board_exit(), 0);

	fake_reset();
	fake_add_pin("49");
	(void)unlink(BBB_SYSFS_GPIO_DIR "/gpio49/value");
	captured_reset();
	CHECK_EQ(board_init(NULL), -1);
	check_messages_follow_convention();

	fake_reset();
	fake_add_pin("49");
	CHECK_EQ(board_init(NULL), 0);
	pin_exported = 1;
	fake_to_dev_full("/unexport");
	captured_reset();
	CHECK_EQ(board_exit(), -1);
	check_messages_follow_convention();
	pin_exported = 0;
}

/*
 * One session with 3 transmit() calls leaves value == "101010", in
 * call order, proving the value fd is opened once and not reset
 * between calls (BRD-02).
 */
static void test_value_fd_opened_once(void)
{
	char buf[MAX_LEN];

	fake_reset();
	fake_add_pin("49");
	CHECK_EQ(board_init(NULL), 0);

	CHECK_EQ(transmit(1, 1, 1), 0);
	CHECK_EQ(transmit(1, 1, 1), 0);
	CHECK_EQ(transmit(1, 1, 1), 0);

	fake_read("/gpio49/value", buf, sizeof(buf));
	CHECK_EQ(strcmp(buf, "101010"), 0);

	CHECK_EQ(board_exit(), 0);
}

/*
 * A session with no transmit() call leaves value empty and the open
 * fd count unchanged once the session closes.
 */
static void test_empty_session(void)
{
	char buf[MAX_LEN];
	long before;

	fake_reset();
	fake_add_pin("49");

	before = count_open_fds();
	CHECK_EQ(board_init(NULL), 0);
	CHECK_EQ(board_exit(), 0);
	CHECK_EQ(count_open_fds(), before);

	fake_read("/gpio49/value", buf, sizeof(buf));
	CHECK_EQ(strcmp(buf, ""), 0);
}

/*
 * Two back-to-back sessions each open and close the value fd once:
 * the open fd count is equal before the first session and after the
 * second, and the second session's transmit() writes "10" on its
 * own once the test truncates value between sessions.
 */
static void test_back_to_back_sessions(void)
{
	char buf[MAX_LEN];
	long before;

	fake_reset();
	fake_add_pin("49");

	before = count_open_fds();

	CHECK_EQ(board_init(NULL), 0);
	CHECK_EQ(transmit(1, 1, 1), 0);
	CHECK_EQ(board_exit(), 0);

	fake_write_file(BBB_SYSFS_GPIO_DIR "/gpio49/value", "");

	CHECK_EQ(board_init(NULL), 0);
	CHECK_EQ(transmit(1, 1, 1), 0);
	CHECK_EQ(board_exit(), 0);

	CHECK_EQ(count_open_fds(), before);

	fake_read("/gpio49/value", buf, sizeof(buf));
	CHECK_EQ(strcmp(buf, "10"), 0);
}

/*
 * transmit() before board_init() and after board_exit() both return
 * -1 and write nothing, since there is no open session.
 */
static void test_transmit_outside_session(void)
{
	char buf[MAX_LEN];

	fake_reset();
	fake_add_pin("49");

	captured_reset();
	CHECK_EQ(transmit(1, 1, 1), -1);
	CHECK(captured_count() > 0);
	fake_read("/gpio49/value", buf, sizeof(buf));
	CHECK_EQ(strcmp(buf, ""), 0);

	CHECK_EQ(board_init(NULL), 0);
	CHECK_EQ(board_exit(), 0);

	captured_reset();
	CHECK_EQ(transmit(1, 1, 1), -1);
	CHECK(captured_count() > 0);
	fake_read("/gpio49/value", buf, sizeof(buf));
	CHECK_EQ(strcmp(buf, ""), 0);
}

/*
 * A board_init() that fails at the direction write (direction ->
 * /dev/full) never reaches the value-fd open, so it leaves the open
 * fd count unchanged.
 */
static void test_failed_init_leaks_no_fd(void)
{
	long before;

	fake_reset();
	fake_add_pin("49");
	fake_to_dev_full("/gpio49/direction");

	before = count_open_fds();
	captured_reset();
	CHECK_EQ(board_init(NULL), -1);
	CHECK(captured_count() > 0);
	CHECK_EQ(count_open_fds(), before);
}

/*
 * After a transmit() failure (value -> /dev/full), board_exit()
 * still closes the value fd, restoring the open fd count, and an
 * immediately following session on a healthy fake tree succeeds.
 */
static void test_session_after_transmit_failure(void)
{
	char buf[MAX_LEN];
	long before;

	fake_reset();
	fake_add_pin("49");
	fake_to_dev_full("/gpio49/value");

	before = count_open_fds();

	CHECK_EQ(board_init(NULL), 0);
	captured_reset();
	CHECK_EQ(transmit(1, 1, 1), -1);
	CHECK(captured_count() > 0);

	CHECK_EQ(board_exit(), 0);
	CHECK_EQ(count_open_fds(), before);

	fake_reset();
	fake_add_pin("49");
	CHECK_EQ(board_init(NULL), 0);
	CHECK_EQ(transmit(1, 1, 1), 0);
	fake_read("/gpio49/value", buf, sizeof(buf));
	CHECK_EQ(strcmp(buf, "10"), 0);
	CHECK_EQ(board_exit(), 0);
}

/*
 * With gpio49 already present, board_init() skips the export write,
 * transmit() still works, and board_exit() leaves unexport and the
 * gpio49 directory untouched (D-17).
 */
static void test_already_exported_pin_left_alone(void)
{
	char buf[MAX_LEN];
	char gpio_dir[MAX_LEN];
	struct stat st;

	fake_reset();
	fake_add_pin("49");

	CHECK_EQ(board_init(NULL), 0);
	fake_read("/export", buf, sizeof(buf));
	CHECK_EQ(strcmp(buf, ""), 0);

	CHECK_EQ(transmit(1, 1, 1), 0);

	CHECK_EQ(board_exit(), 0);
	fake_read("/unexport", buf, sizeof(buf));
	CHECK_EQ(strcmp(buf, ""), 0);

	snprintf(gpio_dir, sizeof(gpio_dir), "%s/gpio49", BBB_SYSFS_GPIO_DIR);
	CHECK_EQ(stat(gpio_dir, &st), 0);
	CHECK(S_ISDIR(st.st_mode));
}

/*
 * When board_init() wrote the pin to export itself (no gpio49
 * present) and then fails opening direction, it writes the pin to
 * unexport before returning -1, and leaks no fd (D-17).
 */
static void test_own_export_undone_on_failure(void)
{
	char buf[MAX_LEN];
	long before;

	fake_reset();

	before = count_open_fds();
	captured_reset();
	CHECK_EQ(board_init(NULL), -1);
	CHECK(captured_count() > 0);
	CHECK_EQ(count_open_fds(), before);

	fake_read("/export", buf, sizeof(buf));
	CHECK_EQ(strcmp(buf, "49"), 0);
	fake_read("/unexport", buf, sizeof(buf));
	CHECK_EQ(strcmp(buf, "49"), 0);
}

/* board_exit() with no open session returns 0 and touches no sysfs
 * file (D-17).
 */
static void test_exit_without_session(void)
{
	char buf[MAX_LEN];

	fake_reset();
	fake_add_pin("49");

	CHECK_EQ(board_exit(), 0);
	fake_read("/unexport", buf, sizeof(buf));
	CHECK_EQ(strcmp(buf, ""), 0);
}

int main(void)
{
	int ret;

	test_lifecycle_with_fake_sysfs();
	test_missing_export_file();
	test_transmit_write_failure_returns();
	test_transmit_missing_value_returns();
	test_board_exit_unexport_failure_returns();
	test_error_messages_follow_convention();
	test_value_fd_opened_once();
	test_empty_session();
	test_back_to_back_sessions();
	test_transmit_outside_session();
	test_failed_init_leaks_no_fd();
	test_session_after_transmit_failure();
	test_already_exported_pin_left_alone();
	test_own_export_undone_on_failure();
	test_exit_without_session();

	ret = MT_REPORT();
	fake_cleanup();

	return ret;
}
