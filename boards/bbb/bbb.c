#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
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

int32_t board_init(const char *config)
{
	int fd;
	size_t n;
	int ret;
	char path[MAX_LEN] = {'\0'};

	snprintf(pin, sizeof(pin), "%s", config ? config : DEFAULT_PIN);

	ret = snprintf(path, sizeof(path), BBB_SYSFS_GPIO_DIR "/export");
	if (ret < 0 || (size_t)ret >= sizeof(path)) {
		syslog(LOG_ERR, "Path too long\n");
		return -1;
	}

	fd = open(path, O_WRONLY);
	if (fd == -1) {
		syslog(LOG_ERR, "Cannot open /sys/class/gpio/export\n");
		return -1;
	}

	n = write(fd, pin, strlen(pin));
	if (n != strlen(pin)) {
		close(fd);
		syslog(LOG_ERR, "Cannot configure GPIO\n");
		return -1;
	}

	ret = close(fd);
	if (ret == -1) {
		syslog(LOG_ERR, "Cannot close /sys/class/gpio/export\n");
		return -1;
	}

	ret = snprintf(path, sizeof(path),
			BBB_SYSFS_GPIO_DIR "/gpio%s/direction", pin);
	if (ret < 0 || (size_t)ret >= sizeof(path)) {
		syslog(LOG_ERR, "Path too long\n");
		return -1;
	}
	fd = open(path, O_WRONLY);
	if (fd == -1) {
		syslog(LOG_ERR, "Cannot open %s\n", path);
		return -1;
	}

	n = write(fd, "out", strlen("out"));
	if (n != strlen("out")) {
		syslog(LOG_ERR, "Cannot configure GPIO as output\n");
		close(fd);
		return -1;
	}

	ret = close(fd);
	if (ret == -1) {
		syslog(LOG_ERR, "Cannot close %s\n", path);
		return -1;
	}

	return 0;
}

int32_t board_exit(void)
{
	int fd;
	size_t n;
	int ret;
	char path[MAX_LEN] = {0};

	ret = snprintf(path, sizeof(path), BBB_SYSFS_GPIO_DIR "/unexport");
	if (ret < 0 || (size_t)ret >= sizeof(path)) {
		syslog(LOG_ERR, "Path too long\n");
		return -1;
	}

	fd = open(path, O_WRONLY);
	if (fd == -1) {
		syslog(LOG_ERR, "Cannot open /sys/class/gpio/unexport\n");
		return -1;
	}

	n = write(fd, pin, strlen(pin));
	if (n != strlen(pin)) {
		syslog(LOG_ERR, "Cannot unexport GPIO\n");
		exit(-1);
	}
	ret = close(fd);
	if (ret == -1) {
		syslog(LOG_ERR, "Cannot close %s\n", path);
		return -1;
	}

	return 0;
}

int32_t transmit(uint32_t nhigh, uint32_t nlow, uint32_t pulse_len)
{
	int fd;
	size_t n;
	int ret;
	char path[MAX_LEN] = {0};

	ret = snprintf(path, sizeof(path), BBB_SYSFS_GPIO_DIR "/gpio%s/value",
			pin);
	if (ret < 0 || (size_t)ret >= sizeof(path)) {
		syslog(LOG_ERR, "Path too long\n");
		exit(-1);
	}
	fd = open(path, O_WRONLY);
	if (fd == -1) {
		syslog(LOG_ERR, "Cannot open %s\n", path);
		exit(-1);
	}
	n = write(fd, "1", 1);
	if (n != strlen("1")) {
		syslog(LOG_ERR, "Cannot write\n");
		exit(-1);
	}
	usleep(pulse_len * nhigh);

	n = write(fd, "0", 1);
	if (n != strlen("0")) {
		syslog(LOG_ERR, "Cannot write\n");
		exit(-1);
	}
	usleep(pulse_len * nlow);

	ret = close(fd);
	if (ret == -1) {
		syslog(LOG_ERR, "Cannot close %s\n", path);
		exit(-1);
	}

	return 0;
}
