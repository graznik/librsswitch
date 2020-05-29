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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#ifdef DEBUG
#include <syslog.h>
#endif
#ifdef RASPBERRY_PI
#include "boards/rpi/rpi.h"
#endif
#ifdef BEAGLEBONE_BLACK
#include "boards/bbb/bbb.h"
#endif
#ifdef STM32F4DISCOVERY
#include "boards/stm32f4discovery/stm32f4discovery.h"
#endif
#ifdef STM32F0DISCOVERY
#include "boards/stm32f0discovery/stm32f0discovery.h"
#endif

#include "librsswitch.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#define MAX_CODEWORD_LEN 13
#define MAX_LEN 100

static void send_tris(char *codeword, uint32_t pulse_len);
static void send_0(uint32_t pulse_len);
static void send_1(uint32_t pulse_len);
static void send_f(uint32_t pulse_len);
static void send_sync(uint32_t pulse_len);
static int32_t rev_008345_send(uint32_t igroup, uint32_t isocket,
			       uint32_t idata);
static int32_t pollin_2605_send(uint32_t igroup, uint32_t isocket,
				uint32_t idata);
static int32_t socket_ctrl(const char *group, const char *socket,
		       const char *data, uint32_t pulse_len);
#ifdef X86_64
static void transmit(uint32_t nhigh, uint32_t nlow, uint32_t pulse_len);
static int32_t board_init(void);
static int32_t board_exit(void);
#endif

#ifdef X86_64
static int32_t board_init(void)
{
	return 0;
}

static int32_t board_exit(void)
{
	return 0;

}

static void transmit(uint32_t nhigh, uint32_t nlow, uint32_t pulse_len)
{
	(void)nhigh;
	(void)nlow;
	(void)pulse_len;
}
#endif

/**
 * Send a code word
 * @param codeword
 * @param ntimes     Number of times to send the code word
 */
static void send_tris(char *codeword, uint32_t pulse_len)
{
	size_t n;

	for (n = 0; n < NTIMES; n++) {
		int i = 0;

		while (codeword[i] != '\0') {
			switch (codeword[i]) {
			case '0':
				send_0(pulse_len);
				break;
			case 'F':
				send_f(pulse_len);
				break;
			case '1':
				send_1(pulse_len);
				break;
			}
			i++;
		}
		send_sync(pulse_len);
	}
}

/**
 * Sends a Tri-State "0" Bit
 *            _     _
 * Waveform: | |___| |___
 */
static void send_0(uint32_t pulse_len)
{
	transmit(1, 3, pulse_len);
	transmit(1, 3, pulse_len);
}

/**
 * Sends a Tri-State "1" Bit
 *            ___   ___
 * Waveform: |   |_|   |_
 */
static void send_1(uint32_t pulse_len)
{
	transmit(3, 1, pulse_len);
	transmit(3, 1, pulse_len);
}

/**
 * Sends a Tri-State "F" Bit
 *            _     ___
 * Waveform: | |___|   |_
 */
static void send_f(uint32_t pulse_len)
{
	transmit(1, 3, pulse_len);
	transmit(3, 1, pulse_len);
}

/**
 * Sends a "Sync" Bit
 *                       _
 * Waveform Protocol 1: | |_______________________________
 *                       _
 * Waveform Protocol 2: | |__________
 */
static void send_sync(uint32_t pulse_len)
{
	transmit(1, 31, pulse_len);
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
#if defined DEBUG && (defined(RASPBERRY_PI) || defined(BEAGLEBONE_BLACK))
		syslog(LOG_ERR, "[-] Unknown socket group %d",  igroup);
#endif
		return -1;
	}
	if (isocket > (nsockets - 1)) {
#if defined DEBUG && (defined(RASPBERRY_PI) || defined(BEAGLEBONE_BLACK))
		syslog(LOG_ERR, "[-] Unknown socket %d", isocket);
#endif
		return -1;
	}

	if (idata > (ndata - 1)) {
#if defined DEBUG && (defined(RASPBERRY_PI) || defined(BEAGLEBONE_BLACK))
		syslog(LOG_ERR, "[-] Unknown switch parameter %d", idata);
#endif
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
#if defined DEBUG && (defined(RASPBERRY_PI) || defined(BEAGLEBONE_BLACK))
		syslog(LOG_ERR, "[-] Unknown socket group %d",  igroup);
#endif
		return -1;
	}
	if (isocket > (nsockets - 1)) {
#if defined DEBUG && (defined(RASPBERRY_PI) || defined(BEAGLEBONE_BLACK))
		syslog(LOG_ERR, "[-] Unknown socket %d", isocket);
#endif
		return -1;
	}

	if (idata > (ndata - 1)) {
#if defined DEBUG && (defined(RASPBERRY_PI) || defined(BEAGLEBONE_BLACK))
		syslog(LOG_ERR, "[-] Unknown switch parameter %d", idata);
#endif
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
	size_t s;
	size_t n;
	char codeword[MAX_CODEWORD_LEN];

	/* Calculate code word size */
	s = strlen(group) + strlen(socket) + strlen(data);

	/* Generate the code word */
	n = snprintf(codeword, s + 1, "%s%s%s", group, socket, data);
	if (n != s) {
#if defined DEBUG && (defined(RASPBERRY_PI) || defined(BEAGLEBONE_BLACK))
		syslog(LOG_ERR, "[-] snprintf()");
#endif
		return -1;
	}

#if defined DEBUG && (defined(RASPBERRY_PI) || defined(BEAGLEBONE_BLACK))
	syslog(LOG_NOTICE, "codeword: %s\n", codeword);
#endif

#ifdef X86_64
	fprintf(stdout, "%s\n", codeword);
#endif

	/* Send the codeword */
	send_tris(codeword, pulse_len);

	return 0;
}

int32_t socket_send(uint32_t dev, uint32_t group,
		    uint32_t socket, uint32_t data)
{
	int32_t ret;

	ret = board_init();
	if (ret < 0) {
#if defined DEBUG && (defined(RASPBERRY_PI) || defined(BEAGLEBONE_BLACK))
		syslog(LOG_ERR, "[-] board_init");
#endif
		return ret;
	}

#if defined DEBUG && (defined(RASPBERRY_PI) || defined(BEAGLEBONE_BLACK))
	syslog(LOG_NOTICE, "dev: %d, group: %d, socket: %d, data: %d",
	       dev, group, socket, data);
#endif

	switch (dev) {
	case REV_008345:
		ret = rev_008345_send(group, socket, data);
#if defined DEBUG && (defined(RASPBERRY_PI) || defined(BEAGLEBONE_BLACK))
		if (ret < 0)
			syslog(LOG_ERR, "[-] rev_008345_send()");
#endif
		break;
	case POLLIN_2605:
		ret = pollin_2605_send(group, socket, data);
#if defined DEBUG && (defined(RASPBERRY_PI) || defined(BEAGLEBONE_BLACK))
		if (ret < 0)
			syslog(LOG_ERR, "[-] pollin_2605_send()");
#endif
		break;
	default:
#if defined DEBUG && (defined(RASPBERRY_PI) || defined(BEAGLEBONE_BLACK))
		syslog(LOG_ERR, "[-] Unknown socket type %d", dev);
#endif
		ret = -1;
	}

#if defined DEBUG && (defined(RASPBERRY_PI) || defined(BEAGLEBONE_BLACK))
	if (board_exit() < 0)
		syslog(LOG_ERR, "[-] board_exit");
#endif
	return ret;
}
