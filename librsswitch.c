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

#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <syslog.h>
#ifdef RASPBERRY_PI
#include "boards/rpi/rpi.h"
#endif
#ifdef RASPBERRY_PI3
#include "boards/rpi/rpi3.h"
#endif
#ifdef BEAGLEBONE_BLACK
#include "boards/bbb/bbb.h"
#endif
#ifdef X86_64
#include "boards/x86_64/x86_64.h"
#endif

#include "librsswitch.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#define NTIMES 10
#define MAX_LEN 100


static int32_t tristate_check(const char *codeword);
static int32_t send_tris(const char *codeword, uint32_t pulse_len);
static int32_t send_0(uint32_t pulse_len);
static int32_t send_1(uint32_t pulse_len);
static int32_t send_f(uint32_t pulse_len);
static int32_t send_sync(uint32_t pulse_len);
static int32_t rev_008345_send(uint32_t igroup, uint32_t isocket,
			       uint32_t idata);
static int32_t pollin_2605_send(uint32_t igroup, uint32_t isocket,
				uint32_t idata);
static int32_t socket_ctrl(const char *group, const char *socket,
			   const char *data, uint32_t pulse_len);

static const char *gpio_config;

/*
 * Serializes every send session (board_init() through board_exit(),
 * including all NTIMES repetitions) across socket_send() and
 * socket_send_codeword(), and also guards gpio_config -- see the
 * "Thread safety" comment in librsswitch.h.
 */
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Rejects codeword unless every byte before the first NUL is one of
 * '0', '1' or uppercase 'F'. Also rejects NULL and the empty string.
 * Called before the first transmit() so a corrupted or malformed
 * codeword never reaches the GPIO, not even partially.
 */
static int32_t tristate_check(const char *codeword)
{
	size_t i;

	if (!codeword || codeword[0] == '\0') {
		syslog(LOG_ERR, "[-] Invalid tristate codeword");
		return -1;
	}

	for (i = 0; codeword[i] != '\0'; i++) {
		if (codeword[i] != '0' && codeword[i] != '1' &&
		    codeword[i] != 'F') {
			syslog(LOG_ERR, "[-] Invalid tristate codeword");
			return -1;
		}
	}

	return 0;
}

/**
 * Send a code word
 * @param codeword
 * @param ntimes     Number of times to send the code word
 */
static int32_t send_tris(const char *codeword, uint32_t pulse_len)
{
	size_t n;

	if (tristate_check(codeword) < 0)
		return -1;

	for (n = 0; n < NTIMES; n++) {
		int i = 0;

		while (codeword[i] != '\0') {
			int32_t ret;

			switch (codeword[i]) {
			case '0':
				ret = send_0(pulse_len);
				break;
			case 'F':
				ret = send_f(pulse_len);
				break;
			case '1':
				ret = send_1(pulse_len);
				break;
			default:
				return -1;
			}
			if (ret < 0)
				return -1;
			i++;
		}
		if (send_sync(pulse_len) < 0)
			return -1;
	}
	return 0;
}

/**
 * Sends a Tri-State "0" Bit
 *            _     _
 * Waveform: | |___| |___
 */
static int32_t send_0(uint32_t pulse_len)
{
	if (transmit(1, 3, pulse_len) < 0)
		return -1;
	return transmit(1, 3, pulse_len);
}

/**
 * Sends a Tri-State "1" Bit
 *            ___   ___
 * Waveform: |   |_|   |_
 */
static int32_t send_1(uint32_t pulse_len)
{
	if (transmit(3, 1, pulse_len) < 0)
		return -1;
	return transmit(3, 1, pulse_len);
}

/**
 * Sends a Tri-State "F" Bit
 *            _     ___
 * Waveform: | |___|   |_
 */
static int32_t send_f(uint32_t pulse_len)
{
	if (transmit(1, 3, pulse_len) < 0)
		return -1;
	return transmit(3, 1, pulse_len);
}

/**
 * Sends a "Sync" Bit
 *                       _
 * Waveform Protocol 1: | |_______________________________
 *                       _
 * Waveform Protocol 2: | |__________
 */
static int32_t send_sync(uint32_t pulse_len)
{
	return transmit(1, 31, pulse_len);
}

/**
 * Configuration struct for the PT2260 encoder
 * @param pt2260     Pointer to a pt2260 instance
 */
static int32_t rev_008345_send(uint32_t igroup, uint32_t isocket,
			       uint32_t idata)
{
	const char * const groups[] = {"1FFF", "F1FF", "FF1F", "FFF1"};
	const char * const sockets[] = {"1FF0", "F1F0", "FF10"};
	const char * const data[] = {"0001", "0010"};
	const uint32_t pulse_len = 350;
	size_t ngroups = ARRAY_SIZE(groups);
	size_t nsockets = ARRAY_SIZE(sockets);
	size_t ndata = ARRAY_SIZE(data);
	int32_t ret;

	if (igroup > (ngroups - 1)) {
		syslog(LOG_ERR, "[-] Unknown socket group %" PRIu32, igroup);
		return -1;
	}
	if (isocket > (nsockets - 1)) {
		syslog(LOG_ERR, "[-] Unknown socket %" PRIu32, isocket);
		return -1;
	}

	if (idata > (ndata - 1)) {
		syslog(LOG_ERR, "[-] Unknown switch parameter %" PRIu32,
		       idata);
		return -1;
	}

	ret = socket_ctrl(groups[igroup], sockets[isocket],
			  data[idata], pulse_len);

	return ret;
}

/**
 * Configuration struct for the PT2262 encoder
 * @param *pt2262     Pointer to a pt2262 instance
 */
static int32_t pollin_2605_send(uint32_t igroup, uint32_t isocket,
				uint32_t idata)
{
	const char *const groups[] = {"FFFFF", "0FFFF", "F0FFF", "00FFF",
				      "FF0FF", "0F0FF", "F00FF", "000FF",
				      "FFF0F", "0FF0F", "F0F0F", "00F0F",
				      "FF00F", "0F00F", "F000F", "0000F",
				      "FFFF0", "0FFF0", "F0FF0", "00FF0",
				      "FF0F0", "0F0F0", "F00F0", "000F0",
				      "FFF00", "0FF00", "F0F00", "00F00",
				      "FF000", "0F000", "F0000", "00000"};
	const char *const sockets[] = {"FFFFF", "0FFFF", "F0FFF", "00FFF",
				       "FF0FF", "0F0FF", "F00FF", "000FF",
				       "FFF0F", "0FF0F", "F0F0F", "00F0F",
				       "FF00F", "0F00F", "F000F", "0000F",
				       "FFFF0", "0FFF0", "F0FF0", "00FF0",
				       "FF0F0", "0F0F0", "F00F0", "000F0",
				       "FFF00", "0FF00", "F0F00", "00F00",
				       "FF000", "0F000", "F0000", "00000"};
	const char * const data[] = {"F0", "0F"}; /* Off, On */
	const uint32_t pulse_len = 350;
	size_t ngroups = ARRAY_SIZE(groups);
	size_t nsockets = ARRAY_SIZE(sockets);
	size_t ndata = ARRAY_SIZE(data);
	int32_t ret;

	if (igroup > (ngroups - 1)) {
		syslog(LOG_ERR, "[-] Unknown socket group %" PRIu32, igroup);
		return -1;
	}
	if (isocket > (nsockets - 1)) {
		syslog(LOG_ERR, "[-] Unknown socket %" PRIu32, isocket);
		return -1;
	}

	if (idata > (ndata - 1)) {
		syslog(LOG_ERR, "[-] Unknown switch parameter %" PRIu32,
		       idata);
		return -1;
	}

	ret = socket_ctrl(groups[igroup], sockets[isocket],
			  data[idata], pulse_len);

	return ret;
}

/**
 * Emulate an encoder chip
 * @param uint32_t group    Socket group
 * @param uint32_t socket   Socket within group
 * @param uint32_t data     Data to send
 */
static int32_t socket_ctrl(const char *group, const char *socket,
			   const char *data, uint32_t pulse_len)
{
	int n;
	char codeword[SOCKET_CODEWORD_MAX_LEN + 1];

	/* Generate the code word */
	n = snprintf(codeword, sizeof(codeword), "%s%s%s", group, socket,
		     data);
	if (n < 0) {
		syslog(LOG_ERR, "[-] snprintf() failed");
		return -1;
	}
	if ((size_t)n >= sizeof(codeword)) {
		syslog(LOG_ERR, "[-] Codeword too long (%zu characters)",
		       (size_t)n);
		return -1;
	}

#ifdef DEBUG
	syslog(LOG_NOTICE, "codeword: %s\n", codeword);
#endif

#ifdef X86_64
	fprintf(stdout, "%s\n", codeword);
#endif

	/* Send the codeword */
	return send_tris(codeword, pulse_len);
}

void socket_gpio_config(const char *config)
{
	if (pthread_mutex_lock(&send_lock) != 0) {
		syslog(LOG_ERR, "[-] pthread_mutex_lock");
		return;
	}
	gpio_config = config;
	pthread_mutex_unlock(&send_lock);
}

int32_t socket_send(uint32_t dev, uint32_t group,
		    uint32_t socket, uint32_t data)
{
	int32_t ret;

	if (pthread_mutex_lock(&send_lock) != 0) {
		syslog(LOG_ERR, "[-] pthread_mutex_lock");
		return -1;
	}

	ret = board_init(gpio_config);
	if (ret < 0) {
		syslog(LOG_ERR, "[-] board_init");
		pthread_mutex_unlock(&send_lock);
		return ret;
	}

#ifdef DEBUG
	syslog(LOG_NOTICE,
	       "dev: %" PRIu32 ", group: %" PRIu32 ", socket: %" PRIu32
	       ", data: %" PRIu32, dev, group, socket, data);
#endif

	switch (dev) {
	case REV_008345:
		ret = rev_008345_send(group, socket, data);
		if (ret < 0)
			syslog(LOG_ERR, "[-] rev_008345_send()");
		break;
	case POLLIN_2605:
		ret = pollin_2605_send(group, socket, data);
		if (ret < 0)
			syslog(LOG_ERR, "[-] pollin_2605_send()");
		break;
	default:
		syslog(LOG_ERR, "[-] Unknown socket type %" PRIu32, dev);
		ret = -1;
	}

	if (board_exit() < 0)
		syslog(LOG_ERR, "[-] board_exit");

	pthread_mutex_unlock(&send_lock);
	return ret;
}

int32_t socket_send_codeword(const char *tristate, uint32_t pulse_len)
{
	size_t len;
	int32_t ret;

	if (!tristate) {
		syslog(LOG_ERR, "[-] Invalid tristate codeword");
		return -1;
	}

	len = strnlen(tristate, SOCKET_CODEWORD_MAX_LEN + 1);
	if (len < SOCKET_CODEWORD_MIN_LEN || len > SOCKET_CODEWORD_MAX_LEN) {
		syslog(LOG_ERR, "[-] Invalid tristate codeword length %zu",
		       len);
		return -1;
	}

	if (tristate_check(tristate) < 0)
		return -1;

	if (pulse_len < SOCKET_PULSE_LEN_MIN_US ||
	    pulse_len > SOCKET_PULSE_LEN_MAX_US) {
		syslog(LOG_ERR, "[-] pulse_len %" PRIu32 " out of range",
		       pulse_len);
		return -1;
	}

	if (pthread_mutex_lock(&send_lock) != 0) {
		syslog(LOG_ERR, "[-] pthread_mutex_lock");
		return -1;
	}

	ret = board_init(gpio_config);
	if (ret < 0) {
		syslog(LOG_ERR, "[-] board_init");
		pthread_mutex_unlock(&send_lock);
		return ret;
	}

#ifdef X86_64
	fprintf(stdout, "%s\n", tristate);
#endif

	ret = send_tris(tristate, pulse_len);

	if (board_exit() < 0)
		syslog(LOG_ERR, "[-] board_exit");

	pthread_mutex_unlock(&send_lock);
	return ret;
}
