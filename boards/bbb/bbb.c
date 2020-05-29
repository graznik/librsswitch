#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef DEBUG
#include <syslog.h>
#endif

#include "bbb.h"

#define MAX_LEN 100

int32_t board_init(void)
{
	int fd;
	size_t n;
	int ret;
	char path[MAX_LEN] = {'\0'};

	fd = open("/sys/class/gpio/export", O_WRONLY);
	if (fd == -1) {
		syslog(LOG_ERR, "Cannot open /sys/class/gpio/export\n");
		return -1;
	}

	n = write(fd, PIN, strlen(PIN));
	if (n != strlen(PIN)) {
		close(fd);
		syslog(LOG_ERR, "Cannot configure GPIO\n");
		return -1;
	}

	ret = close(fd);
	if (ret == -1) {
		syslog(LOG_ERR, "Cannot close /sys/class/gpio/export\n");
		return -1;
	}

	snprintf(path, sizeof(path) - 1, "/sys/class/gpio/gpio%s/direction", PIN);
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

	fd = open("/sys/class/gpio/unexport", O_WRONLY);
	if (fd == -1) {
		syslog(LOG_ERR, "Cannot open /sys/class/gpio/unexport\n");
		return -1;
	}

	n = write(fd, PIN, strlen(PIN));
	if (n != strlen(PIN)) {
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

void transmit(uint32_t nhigh, uint32_t nlow, uint32_t pulse_len)
{
	int fd;
	size_t n;
	int ret;
	char path[MAX_LEN] = {0};

	snprintf(path, sizeof(path) - 1, "/sys/class/gpio/gpio%s/value", PIN);
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
}
