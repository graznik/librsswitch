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
 * Baseline: with gpio49 pre-created (simulating a udev-managed
 * board), board_init()/transmit()/board_exit() run end to end and
 * leave the expected content in each sysfs file.
 */
static void test_lifecycle_with_fake_sysfs(void)
{
	char buf[MAX_LEN];

	fake_reset();
	fake_add_pin("49");

	CHECK_EQ(board_init(NULL), 0);
	fake_read("/export", buf, sizeof(buf));
	CHECK_EQ(strcmp(buf, "49"), 0);
	fake_read("/gpio49/direction", buf, sizeof(buf));
	CHECK_EQ(strcmp(buf, "out"), 0);

	CHECK_EQ(transmit(1, 1, 1), 0);
	fake_read("/gpio49/value", buf, sizeof(buf));
	CHECK_EQ(strcmp(buf, "10"), 0);

	CHECK_EQ(board_exit(), 0);
	fake_read("/unexport", buf, sizeof(buf));
	CHECK_EQ(strcmp(buf, "49"), 0);
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
 * process; the test keeps running afterwards.
 */
static void test_transmit_write_failure_returns(void)
{
	fake_reset();
	fake_add_pin("49");
	CHECK_EQ(board_init(NULL), 0);

	fake_to_dev_full("/gpio49/value");
	CHECK_EQ(transmit(1, 1, 1), -1);
	CHECK(captured_count() > 0);

	CHECK_EQ(board_exit(), 0);
}

/*
 * A missing value file makes transmit()'s open() fail, returning -1
 * instead of terminating the process.
 */
static void test_transmit_missing_value_returns(void)
{
	fake_reset();
	fake_add_pin("49");
	CHECK_EQ(board_init(NULL), 0);

	(void)unlink(BBB_SYSFS_GPIO_DIR "/gpio49/value");
	CHECK_EQ(transmit(1, 1, 1), -1);
	CHECK(captured_count() > 0);

	CHECK_EQ(board_exit(), 0);
}

/*
 * An unexport file replaced by a symlink to /dev/full makes
 * board_exit()'s write() fail (ENOSPC), returning -1 instead of
 * terminating the process.
 */
static void test_board_exit_unexport_failure_returns(void)
{
	fake_reset();
	fake_add_pin("49");
	CHECK_EQ(board_init(NULL), 0);

	fake_to_dev_full("/unexport");
	CHECK_EQ(board_exit(), -1);
	CHECK(captured_count() > 0);
}

int main(void)
{
	int ret;

	test_lifecycle_with_fake_sysfs();
	test_missing_export_file();
	test_transmit_write_failure_returns();
	test_transmit_missing_value_returns();
	test_board_exit_unexport_failure_returns();

	ret = MT_REPORT();
	fake_cleanup();

	return ret;
}
