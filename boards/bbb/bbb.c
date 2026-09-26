#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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

/*
 * Set to 1 only when this board_init() call is the one that wrote
 * pin to export -- never for a pin that was already exported before
 * board_init() ran. board_exit() unexports the pin only when this
 * is 1, so a pin set up beforehand by udev or an administrator is
 * left alone (D-17).
 */
static int pin_exported;

/*
 * Undoes this board_init()'s own export, if it made one, logging
 * (but not failing) if the unexport write itself fails -- called
 * from a board_init() failure path that ran after a successful
 * export, so the caller is already returning -1 for a different
 * reason.
 */
static void undo_own_export(void)
{
	int fd;
	ssize_t n;
	int ret;
	char path[MAX_LEN] = {0};

	if (!pin_exported)
		return;

	pin_exported = 0;

	ret = snprintf(path, sizeof(path), BBB_SYSFS_GPIO_DIR "/unexport");
	if (ret < 0 || (size_t)ret >= sizeof(path)) {
		syslog(LOG_ERR, "[-] Path too long: %s", path);
		return;
	}

	fd = open(path, O_WRONLY);
	if (fd == -1) {
		syslog(LOG_ERR, "[-] Cannot open %s: %s", path, strerror(errno));
		return;
	}

	n = write(fd, pin, strlen(pin));
	if (n != (ssize_t)strlen(pin))
		syslog(LOG_ERR, "[-] Cannot write %s: %s", path, strerror(errno));

	close(fd);
}

/*
 * Parses config as a decimal GPIO number and writes the canonical
 * form (no leading zeros) into pin. NULL uses the compiled-in
 * default (49). A non-NULL config must be non-empty, contain only
 * the digits '0'-'9' (rejecting a sign, whitespace, a hex prefix or
 * a path separator before it ever reaches strtoul()), and parse to
 * a value that fits an int (the kernel numbers GPIOs with int, per
 * Pitfall 4's overflow/trailing-garbage idiom). Returns 0 on
 * success, -1 (with an error logged, and no sysfs access attempted)
 * on a rejected config.
 */
static int32_t parse_pin(const char *config)
{
	const char *p;
	char *end;
	unsigned long val;

	if (config == NULL) {
		snprintf(pin, sizeof(pin), "%s", DEFAULT_PIN);
		return 0;
	}

	if (config[0] == '\0')
		goto invalid;

	for (p = config; *p != '\0'; p++) {
		if (*p < '0' || *p > '9')
			goto invalid;
	}

	errno = 0;
	val = strtoul(config, &end, 10);
	if (*end != '\0' || errno == ERANGE || val > INT_MAX)
		goto invalid;

	snprintf(pin, sizeof(pin), "%lu", val);

	return 0;

invalid:
	syslog(LOG_ERR, "[-] Invalid GPIO number '%s'", config);
	return -1;
}

/*
 * config: a decimal GPIO number (default 49 when config is NULL;
 * leading zeros allowed, e.g. "049"; at most INT_MAX). A pin that is
 * already exported (gpio<pin> already exists) is used as is and
 * never unexported by board_exit(). A pin this call exports itself
 * is unexported by board_exit().
 */
int32_t board_init(const char *config)
{
	int fd;
	ssize_t n;
	int ret;
	char path[MAX_LEN] = {'\0'};
	struct stat st;

	if (parse_pin(config) == -1)
		return -1;
	pin_exported = 0;

	ret = snprintf(path, sizeof(path), BBB_SYSFS_GPIO_DIR "/gpio%s", pin);
	if (ret < 0 || (size_t)ret >= sizeof(path)) {
		syslog(LOG_ERR, "[-] Path too long: %s", path);
		return -1;
	}

	if (stat(path, &st) == -1 || !S_ISDIR(st.st_mode)) {
		ret = snprintf(path, sizeof(path), BBB_SYSFS_GPIO_DIR "/export");
		if (ret < 0 || (size_t)ret >= sizeof(path)) {
			syslog(LOG_ERR, "[-] Path too long: %s", path);
			return -1;
		}

		fd = open(path, O_WRONLY);
		if (fd == -1) {
			syslog(LOG_ERR, "[-] Cannot open %s: %s", path,
				strerror(errno));
			return -1;
		}

		n = write(fd, pin, strlen(pin));
		if (n != (ssize_t)strlen(pin) && errno != EBUSY) {
			close(fd);
			syslog(LOG_ERR, "[-] Cannot write %s: %s", path,
				strerror(errno));
			return -1;
		}
		if (n == (ssize_t)strlen(pin))
			pin_exported = 1;

		ret = close(fd);
		if (ret == -1) {
			syslog(LOG_ERR, "[-] Cannot close %s: %s", path,
				strerror(errno));
			return -1;
		}
	}

	ret = snprintf(path, sizeof(path),
			BBB_SYSFS_GPIO_DIR "/gpio%s/direction", pin);
	if (ret < 0 || (size_t)ret >= sizeof(path)) {
		syslog(LOG_ERR, "[-] Path too long: %s", path);
		undo_own_export();
		return -1;
	}
	fd = open(path, O_WRONLY);
	if (fd == -1) {
		syslog(LOG_ERR, "[-] Cannot open %s: %s", path, strerror(errno));
		undo_own_export();
		return -1;
	}

	n = write(fd, "out", strlen("out"));
	if (n != (ssize_t)strlen("out")) {
		syslog(LOG_ERR, "[-] Cannot write %s: %s", path, strerror(errno));
		close(fd);
		undo_own_export();
		return -1;
	}

	ret = close(fd);
	if (ret == -1) {
		syslog(LOG_ERR, "[-] Cannot close %s: %s", path, strerror(errno));
		undo_own_export();
		return -1;
	}

	ret = snprintf(path, sizeof(path), BBB_SYSFS_GPIO_DIR "/gpio%s/value",
			pin);
	if (ret < 0 || (size_t)ret >= sizeof(path)) {
		syslog(LOG_ERR, "[-] Path too long: %s", path);
		undo_own_export();
		return -1;
	}

	value_fd = open(path, O_WRONLY);
	if (value_fd == -1) {
		syslog(LOG_ERR, "[-] Cannot open %s: %s", path, strerror(errno));
		undo_own_export();
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

	if (!pin_exported)
		return 0;

	pin_exported = 0;

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
