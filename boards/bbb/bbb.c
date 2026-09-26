#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <syslog.h>

#include "bbb.h"

#define MAX_LEN 100
#define DEFAULT_PIN "49"

/*
 * Base directory for every sysfs GPIO path this board touches.
 * Overridden only by tests/test_bbb.c, which points it at a fake
 * tree so board_init(), transmit() and board_exit() -- including
 * their error paths -- can be driven end to end on any Linux host
 * without a BeagleBone. No production build defines this, so every
 * shipped BBB build uses the real /sys/class/gpio.
 */
#ifndef BBB_SYSFS_GPIO_DIR
#define BBB_SYSFS_GPIO_DIR "/sys/class/gpio"
#endif

static char pin[16];

/*
 * The sysfs value fd for the current session, or -1 when no session
 * is open. Opened once by board_init() and kept open across every
 * transmit() call, instead of opening and closing it for every
 * pulse pair -- two extra system calls per pulse inside the
 * timing-critical loop, and a failed open() in the middle of a
 * codeword used to abort it (BRD-02).
 */
static int value_fd = -1;
static char value_path[MAX_LEN];

int32_t board_init(const char *config)
{
	int fd;
	ssize_t n;
	int ret;
	char path[MAX_LEN] = {'\0'};

	snprintf(pin, sizeof(pin), "%s", config ? config : DEFAULT_PIN);

	ret = snprintf(path, sizeof(path), BBB_SYSFS_GPIO_DIR "/export");
	if (ret < 0 || (size_t)ret >= sizeof(path)) {
		syslog(LOG_ERR, "[-] Path too long: %s", path);
		return -1;
	}

	fd = open(path, O_WRONLY);
	if (fd == -1) {
		syslog(LOG_ERR, "[-] Cannot open %s: %s", path, strerror(errno));
		return -1;
	}

	n = write(fd, pin, strlen(pin));
	if (n != (ssize_t)strlen(pin)) {
		close(fd);
		syslog(LOG_ERR, "[-] Cannot write %s: %s", path, strerror(errno));
		return -1;
	}

	ret = close(fd);
	if (ret == -1) {
		syslog(LOG_ERR, "[-] Cannot close %s: %s", path, strerror(errno));
		return -1;
	}

	ret = snprintf(path, sizeof(path),
			BBB_SYSFS_GPIO_DIR "/gpio%s/direction", pin);
	if (ret < 0 || (size_t)ret >= sizeof(path)) {
		syslog(LOG_ERR, "[-] Path too long: %s", path);
		return -1;
	}
	fd = open(path, O_WRONLY);
	if (fd == -1) {
		syslog(LOG_ERR, "[-] Cannot open %s: %s", path, strerror(errno));
		return -1;
	}

	n = write(fd, "out", strlen("out"));
	if (n != (ssize_t)strlen("out")) {
		syslog(LOG_ERR, "[-] Cannot write %s: %s", path, strerror(errno));
		close(fd);
		return -1;
	}

	ret = close(fd);
	if (ret == -1) {
		syslog(LOG_ERR, "[-] Cannot close %s: %s", path, strerror(errno));
		return -1;
	}

	ret = snprintf(path, sizeof(path), BBB_SYSFS_GPIO_DIR "/gpio%s/value",
			pin);
	if (ret < 0 || (size_t)ret >= sizeof(path)) {
		syslog(LOG_ERR, "[-] Path too long: %s", path);
		return -1;
	}

	value_fd = open(path, O_WRONLY);
	if (value_fd == -1) {
		syslog(LOG_ERR, "[-] Cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	snprintf(value_path, sizeof(value_path), "%s", path);

	return 0;
}

int32_t board_exit(void)
{
	int fd;
	ssize_t n;
	int ret;
	char path[MAX_LEN] = {0};

	if (value_fd != -1) {
		ret = close(value_fd);
		value_fd = -1;
		if (ret == -1) {
			syslog(LOG_ERR, "[-] Cannot close %s: %s", value_path,
				strerror(errno));
			return -1;
		}
	}

	ret = snprintf(path, sizeof(path), BBB_SYSFS_GPIO_DIR "/unexport");
	if (ret < 0 || (size_t)ret >= sizeof(path)) {
		syslog(LOG_ERR, "[-] Path too long: %s", path);
		return -1;
	}

	fd = open(path, O_WRONLY);
	if (fd == -1) {
		syslog(LOG_ERR, "[-] Cannot open %s: %s", path, strerror(errno));
		return -1;
	}

	n = write(fd, pin, strlen(pin));
	if (n != (ssize_t)strlen(pin)) {
		close(fd);
		syslog(LOG_ERR, "[-] Cannot write %s: %s", path, strerror(errno));
		return -1;
	}
	ret = close(fd);
	if (ret == -1) {
		syslog(LOG_ERR, "[-] Cannot close %s: %s", path, strerror(errno));
		return -1;
	}

	return 0;
}

int32_t transmit(uint32_t nhigh, uint32_t nlow, uint32_t pulse_len)
{
	ssize_t n;

	if (value_fd == -1) {
		syslog(LOG_ERR, "[-] transmit() without board_init()");
		return -1;
	}

	n = write(value_fd, "1", 1);
	if (n != (ssize_t)strlen("1")) {
		syslog(LOG_ERR, "[-] Cannot write %s: %s", value_path,
			strerror(errno));
		return -1;
	}
	usleep(pulse_len * nhigh);

	n = write(value_fd, "0", 1);
	if (n != (ssize_t)strlen("0")) {
		syslog(LOG_ERR, "[-] Cannot write %s: %s", value_path,
			strerror(errno));
		return -1;
	}
	usleep(pulse_len * nlow);

	return 0;
}
