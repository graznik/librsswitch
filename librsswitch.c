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

/**
 * Send a code word
 * @param codeword   /^[10FS]*$/  -> see get_codeword
 * @param ntimes     Number of times to send the code word
 */
void send_tris(char *codeword, int ntimes, uint pulse_len)
{
	int n;

	for (n = 0; n < ntimes; n++) {
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
void send_0(uint pulse_len)
{
	transmit(1, 3, pulse_len);
	transmit(1, 3, pulse_len);
}

/**
 * Sends a Tri-State "1" Bit
 *            ___   ___
 * Waveform: |   |_|   |_
 */
void send_1(uint pulse_len)
{
	transmit(3, 1, pulse_len);
	transmit(3, 1, pulse_len);
}

/**
 * Sends a Tri-State "F" Bit
 *            _     ___
 * Waveform: | |___|   |_
 */
void send_f(uint pulse_len)
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
void send_sync(uint pulse_len)
{
	transmit(1, 31, pulse_len);
}

void transmit(int nhigh, int nlow, uint pulse_len)
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
int pt2260_init(struct encoder *pt2260)
{
	char *groups[] = {"1FFF", "F1FF", "FF1F", "FFF1"};
	char *sockets[] = {"1FF0", "F1F0", "FF10"};
	char *data[] = {"0001", "0010"};
	uint pulse_len = 350;
	int i;

	/* Four possible switch groups */
	pt2260->ngroups = sizeof(groups) / sizeof(groups[0]);
	pt2260->groups = malloc(pt2260->ngroups * sizeof(char *));
	if (pt2260->groups == NULL) {
		fputs("Error: Cannot malloc\n", stdout);
		return -1;
	}
	/* Three possible switches per group */
	pt2260->nsockets = sizeof(sockets) / sizeof(sockets[0]);
	pt2260->sockets = malloc(pt2260->nsockets * sizeof(char *));
	if (pt2260->sockets == NULL) {
		fputs("Error: Cannot malloc\n", stdout);
		return -1;
	}

	/* Data is either "On" or "Off" */
	pt2260->ndata = 2;
	pt2260->data = malloc(pt2260->ndata * sizeof(char *));
	if (pt2260->data == NULL) {
		fputs("Error: Cannot malloc\n", stdout);
		return -1;
	}

	for (i = 0; i < pt2260->ngroups; i++)
		pt2260->groups[i] = groups[i];

	for (i = 0; i < pt2260->nsockets; i++)
		pt2260->sockets[i] =  sockets[i];


	for (i = 0; i < pt2260->ndata; i++)
		pt2260->data[i] = data[i];

	pt2260->pulse_len = pulse_len;

	return 0;
}

/**
 * Configuration struct for the PT2262 encoder
 * @param *pt2262     Pointer to a pt2262 instance
 */
int pt2262_init(struct encoder *pt2262)
{
	char *groups[] = {"FFFF", "0FFF", "F0FF", "00FF",
			  "FF0F", "0F0F", "F00F", "000F",
			  "FFF0", "0FF0", "F0F0", "00F0",
			  "FF00", "0F00", "F000", "0000"};
	char *sockets[] = {"F0FF", "FF0F", "FFF0", "FFFF"};
	char *data[] = {"FFF0", "FF0F"};
	uint pulse_len = 350;
	int i;

	/* 16 possible switch groups (A-P in Intertechno code) */
	pt2262->ngroups = sizeof(groups) / sizeof(groups[0]);
	pt2262->groups = malloc(pt2262->ngroups * sizeof(char *));
	if (pt2262->groups == NULL) {
		fputs("Error: Cannot malloc\n", stdout);
		return -1;
	}

	/* Four possible switches per group */
	pt2262->nsockets = sizeof(sockets) / sizeof(sockets[0]);
	pt2262->sockets = malloc(pt2262->nsockets * sizeof(char *));
	if (pt2262->sockets == NULL) {
		fputs("Error: Cannot malloc\n", stdout);
		return -1;
	}

	/* Data is either "On" or "Off" */
	pt2262->ndata = 2;
	pt2262->data = malloc(pt2262->ndata * sizeof(char *));
	if (pt2262->data == NULL) {
		fputs("Error: Cannot malloc\n", stdout);
		return -1;
	}

	for (i = 0; i < pt2262->ngroups; i++)
		pt2262->groups[i] = groups[i];

	for (i = 0; i < pt2262->nsockets; i++)
		pt2262->sockets[i] =  sockets[i];

	for (i = 0; i < pt2262->ndata; i++)
		pt2262->data[i] = data[i];

	pt2262->pulse_len = pulse_len;

	return 0;
}

void free_encoder(struct encoder *enc)
{
	free(enc->groups);
	free(enc->sockets);
	free(enc->data);
}

/**
 * Emulate an encoder chip
 * @param *enc          Pointer to an encoder instance
 * @param uint group    Socket group
 * @param uint socket   Socket within group
 * @param uint data     Data to send
 */
int socket_ctrl(struct encoder *enc, uint group, uint socket, uint data)
{
	/* Calculate code word size */
	size_t s = strlen(enc->groups[group]) +
		strlen(enc->sockets[socket]) +
		strlen(enc->data[data]);
	char codeword[s];

	/* Generate the code word */
	snprintf(codeword, s + 1, "%s%s%s",
		 enc->groups[group],
		 enc->sockets[socket],
		 enc->data[data]);

#ifdef DEBUG
	syslog(LOG_NOTICE, "codeword: %s\n", codeword);
#endif

	/* Initialize the IO pin */
	if (!bcm2835_init()) {
		fputs("Cannot init bcm2835\n", stdout);
		return -1;
	}

	/* Set the pin to be an output */
	bcm2835_gpio_fsel(PIN, BCM2835_GPIO_FSEL_OUTP);

	/* Send the codeword */
	send_tris(codeword, 10, enc->pulse_len);

	return 0;
}

int socket_send(uint dev, uint group, uint socket, uint data)
{
	struct encoder enc;

#ifdef DEBUG
	syslog(LOG_NOTICE, "dev: %d, group: %d, socket: %d, data: %d",
	       dev, group, socket, data);
#endif

	switch (dev) {
	case 0:
		pt2260_init(&enc);
		break;
	case 1:
		pt2262_init(&enc);
		break;
	default:
		syslog(LOG_ERR, "Received unknown socket type %d", dev);
		return EXIT_FAILURE;
	}

	if ((group > (enc.ngroups - 1)) ||
	    (socket > (enc.nsockets - 1)) ||
	    (data > (enc.ndata - 1))) {
		syslog(LOG_ERR, "Received unknown parameter");
		return EXIT_FAILURE;
	}

	socket_ctrl(&enc, group, socket, data);

	free_encoder(&enc);

	return EXIT_SUCCESS;
}
