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

#include "librsswitch.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#define MAX_CODEWORD_LEN 13
#define NTIMES 10

static void send_tris(char *codeword, uint pulse_len);
static void send_0(uint pulse_len);
static void send_1(uint pulse_len);
static void send_f(uint pulse_len);
static void send_sync(uint pulse_len);
static void transmit(int nhigh, int nlow, uint pulse_len);
static int pt2260_send(uint igroup, uint isocket, uint idata);
static int pt2262_send(uint igroup, uint isocket, uint idata);
static int socket_ctrl(const char *group, const char *socket,
		       const char *data, uint pulse_len);

/**
 * Send a code word
 * @param codeword   /^[10FS]*$/  -> see get_codeword
 * @param ntimes     Number of times to send the code word
 */
static void send_tris(char *codeword, uint pulse_len)
{
	int n;

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
static void send_0(uint pulse_len)
{
	transmit(1, 3, pulse_len);
	transmit(1, 3, pulse_len);
}

/**
 * Sends a Tri-State "1" Bit
 *            ___   ___
 * Waveform: |   |_|   |_
 */
static void send_1(uint pulse_len)
{
	transmit(3, 1, pulse_len);
	transmit(3, 1, pulse_len);
}

/**
 * Sends a Tri-State "F" Bit
 *            _     ___
 * Waveform: | |___|   |_
 */
static void send_f(uint pulse_len)
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
static void send_sync(uint pulse_len)
{
	transmit(1, 31, pulse_len);
}

static void transmit(int nhigh, int nlow, uint pulse_len)
{
	bcm2835_gpio_write(PIN, HIGH);
	delayMicroseconds(pulse_len * nhigh);
	bcm2835_gpio_write(PIN, LOW);
	delayMicroseconds(pulse_len * nlow);
}

/**
 * Configuration struct for the PT2260 encoder
 * @param pt2260     Pointer to a pt2260 instance
 */
static int pt2260_send(uint igroup, uint isocket, uint idata)
{
	const char * const groups[] = {"1FFF", "F1FF", "FF1F", "FFF1"};
	const char * const sockets[] = {"1FF0", "F1F0", "FF10"};
	const char * const data[] = {"0001", "0010"};
	const uint pulse_len = 350;
	size_t ngroups = ARRAY_SIZE(groups);
	size_t nsockets = ARRAY_SIZE(sockets);
	size_t ndata = ARRAY_SIZE(data);

	if ((igroup > (ngroups - 1)) ||
	    (isocket > (nsockets - 1)) ||
	    (idata > (ndata - 1))) {
		syslog(LOG_ERR, "Received unknown parameter");
		return 1;
	}

	socket_ctrl(groups[igroup], sockets[isocket], data[idata], pulse_len);

	return 0;
}

/**
 * Configuration struct for the PT2262 encoder
 * @param *pt2262     Pointer to a pt2262 instance
 */
static int pt2262_send(uint igroup, uint isocket, uint idata)
{
	const char *const groups[] = {"FFFF", "0FFF", "F0FF", "00FF",
				      "FF0F", "0F0F", "F00F", "000F",
				      "FFF0", "0FF0", "F0F0", "00F0",
				      "FF00", "0F00", "F000", "0000"};
	const char * const sockets[] = {"F0FF", "FF0F", "FFF0", "FFFF"};
	const char * const data[] = {"FFF0", "FF0F"};
	const uint pulse_len = 350;
	size_t ngroups = ARRAY_SIZE(groups);
	size_t nsockets = ARRAY_SIZE(sockets);
	size_t ndata = ARRAY_SIZE(data);

	if ((igroup > (ngroups - 1)) ||
	    (isocket > (nsockets - 1)) ||
	    (idata > (ndata - 1))) {
		syslog(LOG_ERR, "Received unknown parameter");
		return 1;
	}

	socket_ctrl(groups[igroup], sockets[isocket], data[idata], pulse_len);

	return 0;
}

/**
 * Emulate an encoder chip
 * @param *enc          Pointer to an encoder instance
 * @param uint group    Socket group
 * @param uint socket   Socket within group
 * @param uint data     Data to send
 */
static int socket_ctrl(const char *group, const char *socket,
		       const char *data, uint pulse_len)
{
	size_t s;
	char codeword[MAX_CODEWORD_LEN];

	/* Calculate code word size */
	s = strlen(group) + strlen(socket) + strlen(data);

	/* Generate the code word */
	snprintf(codeword, s + 1, "%s%s%s", group, socket, data);

#ifdef DEBUG
	syslog(LOG_NOTICE, "codeword: %s\n", codeword);
#endif

	/* Initialize the IO pin */
	if (!bcm2835_init()) {
		syslog(LOG_ERR, "Cannot init bcm2835\n");
		return -1;
	}

	/* Set the pin to be an output */
	bcm2835_gpio_fsel(PIN, BCM2835_GPIO_FSEL_OUTP);

	/* Send the codeword */
	send_tris(codeword, pulse_len);

	return 0;
}

int socket_send(uint dev, uint group, uint socket, uint data)
{

#ifdef DEBUG
	syslog(LOG_NOTICE, "dev: %d, group: %d, socket: %d, data: %d",
	       dev, group, socket, data);
#endif

	switch (dev) {
	case 0:
		pt2260_send(group, socket, data);
		break;
	case 1:
		pt2262_send(group, socket, data);
		break;
	default:
		syslog(LOG_ERR, "Received unknown socket type %d", dev);
		return 1;
	}

	return 0;
}
