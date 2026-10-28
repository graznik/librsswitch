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

#include "boards/board.h"
#include "librsswitch.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#define NTIMES 10
#define MAX_LEN 100


static int32_t address_check(uint32_t dev, uint32_t group, uint32_t socket,
			     uint32_t data);
static int32_t dev_limits(uint32_t dev, size_t *ngroups, size_t *nsockets,
			  size_t *ndata);
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
static int32_t fail(enum rss_err err);
static enum rss_err cause_from_board(int32_t ret);

static const char *gpio_config;

/*
 * The cause of the calling thread's last send, returned by
 * rss_last_error(). The GNU storage class is used because the C11
 * keyword is rejected by -pedantic under -std=c99.
 */
static __thread enum rss_err last_error;

/*
 * Serializes every send session (board_init() through board_exit(),
 * including all NTIMES repetitions) across rss_send() and
 * rss_send_codeword(), and also guards gpio_config -- see the
 * "Thread safety" comment in librsswitch.h.
 */
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Tri-state code parts of the two supported devices, indexed by
 * group, socket and data. Their sizes define the valid address ranges
 * for each device (REV 008345: group 0-3, socket 0-2; Pollin 2605:
 * group 0-31, socket 0-31; both: data 0-1).
 */
static const char * const rev_008345_groups[] = {"1FFF", "F1FF", "FF1F",
						  "FFF1"};
static const char * const rev_008345_sockets[] = {"1FF0", "F1F0", "FF10"};
static const char * const rev_008345_data[] = {"0001", "0010"};

static const char * const pollin_2605_groups[] = {
	"FFFFF", "0FFFF", "F0FFF", "00FFF",
	"FF0FF", "0F0FF", "F00FF", "000FF",
	"FFF0F", "0FF0F", "F0F0F", "00F0F",
	"FF00F", "0F00F", "F000F", "0000F",
	"FFFF0", "0FFF0", "F0FF0", "00FF0",
	"FF0F0", "0F0F0", "F00F0", "000F0",
	"FFF00", "0FF00", "F0F00", "00F00",
	"FF000", "0F000", "F0000", "00000"};
static const char * const pollin_2605_sockets[] = {
	"FFFFF", "0FFFF", "F0FFF", "00FFF",
	"FF0FF", "0F0FF", "F00FF", "000FF",
	"FFF0F", "0FF0F", "F0F0F", "00F0F",
	"FF00F", "0F00F", "F000F", "0000F",
	"FFFF0", "0FFF0", "F0FF0", "00FF0",
	"FF0F0", "0F0F0", "F00F0", "000F0",
	"FFF00", "0FF00", "F0F00", "00F00",
	"FF000", "0F000", "F0000", "00000"};
static const char * const pollin_2605_data[] = {"F0", "0F"}; /* Off, On */

/*
 * Fills ngroups, nsockets and ndata with dev's table sizes, the same
 * bounds address_check() enforces -- or returns -1 for an unknown dev
 * without logging. Both address_check() and the public
 * rss_addr_limits() call this, so the two can never see different
 * bounds for the same dev: each decides for itself whether and what
 * to log about the result.
 */
static int32_t dev_limits(uint32_t dev, size_t *ngroups, size_t *nsockets,
			  size_t *ndata)
{
	switch (dev) {
	case RSS_REV_008345:
		*ngroups = ARRAY_SIZE(rev_008345_groups);
		*nsockets = ARRAY_SIZE(rev_008345_sockets);
		*ndata = ARRAY_SIZE(rev_008345_data);
		return 0;
	case RSS_POLLIN_2605:
		*ngroups = ARRAY_SIZE(pollin_2605_groups);
		*nsockets = ARRAY_SIZE(pollin_2605_sockets);
		*ndata = ARRAY_SIZE(pollin_2605_data);
		return 0;
	default:
		return -1;
	}
}

/*
 * Rejects an unknown device or an out-of-range group, socket or data
 * for the given device. rss_send() calls this before send_lock and
 * board_init(), so a rejected address never touches the board.
 */
static int32_t address_check(uint32_t dev, uint32_t group, uint32_t socket,
			     uint32_t data)
{
	size_t ngroups, nsockets, ndata;

	if (dev_limits(dev, &ngroups, &nsockets, &ndata) < 0) {
		syslog(LOG_ERR, "[-] Unknown socket type %" PRIu32, dev);
		return -1;
	}

	if (group >= ngroups) {
		syslog(LOG_ERR, "[-] Unknown socket group %" PRIu32, group);
		return -1;
	}
	if (socket >= nsockets) {
		syslog(LOG_ERR, "[-] Unknown socket %" PRIu32, socket);
		return -1;
	}
	if (data >= ndata) {
		syslog(LOG_ERR, "[-] Unknown switch parameter %" PRIu32,
		       data);
		return -1;
	}

	return 0;
}

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

/*
 * Records err as the cause of the calling thread's failed send and
 * returns -1, the value both public send functions return on failure.
 */
static int32_t fail(enum rss_err err)
{
	last_error = err;
	return -1;
}

/*
 * Turns the value a board returned into a cause. A board returns 0, -1
 * or -(int32_t)SOCKET_ERR_x for a failure it can name; -1 is
 * RSS_ERR_OTHER. A value outside that range is RSS_ERR_OTHER too.
 * The range is tested before ret is negated, so INT32_MIN is never
 * negated.
 */
static enum rss_err cause_from_board(int32_t ret)
{
	if (ret >= 0)
		return RSS_ERR_NONE;
	if (ret < -(int32_t)RSS_ERR_IO)
		return RSS_ERR_OTHER;
	return (enum rss_err)-ret;
}

/**
 * Sends a tri-state code word NTIMES times, each repetition followed
 * by a sync pulse.
 * @param codeword     NUL-terminated string of '0', '1' and 'F'
 * @param pulse_len    Pulse length in microseconds
 * @return 0 on success, -1 or a negative board cause on failure
 */
static int32_t send_tris(const char *codeword, uint32_t pulse_len)
{
	size_t n;
	int32_t ret;

	if (tristate_check(codeword) < 0)
		return -1;

	board_codeword(codeword);

	for (n = 0; n < NTIMES; n++) {
		int i = 0;

		while (codeword[i] != '\0') {
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
				return ret;
			i++;
		}
		ret = send_sync(pulse_len);
		if (ret < 0)
			return ret;
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
	int32_t ret;

	ret = transmit(1, 3, pulse_len);
	if (ret < 0)
		return ret;
	return transmit(1, 3, pulse_len);
}

/**
 * Sends a Tri-State "1" Bit
 *            ___   ___
 * Waveform: |   |_|   |_
 */
static int32_t send_1(uint32_t pulse_len)
{
	int32_t ret;

	ret = transmit(3, 1, pulse_len);
	if (ret < 0)
		return ret;
	return transmit(3, 1, pulse_len);
}

/**
 * Sends a Tri-State "F" Bit
 *            _     ___
 * Waveform: | |___|   |_
 */
static int32_t send_f(uint32_t pulse_len)
{
	int32_t ret;

	ret = transmit(1, 3, pulse_len);
	if (ret < 0)
		return ret;
	return transmit(3, 1, pulse_len);
}

/**
 * Sends a "Sync" Bit
 *            _
 * Waveform: | |_______________________________
 */
static int32_t send_sync(uint32_t pulse_len)
{
	return transmit(1, 31, pulse_len);
}

/**
 * Emulates the PT2260 encoder used by REV 008345.
 * @param igroup    Socket group, valid range 0-3
 * @param isocket   Socket within the group, valid range 0-2
 * @param idata     0 = off, 1 = on
 * @return 0 on success, -1 or a negative board cause on failure
 *
 * igroup, isocket and idata are already checked by address_check()
 * before rss_send() reaches this function -- it indexes the
 * file-scope tables directly.
 */
static int32_t rev_008345_send(uint32_t igroup, uint32_t isocket,
			       uint32_t idata)
{
	const uint32_t pulse_len = 350;

	return socket_ctrl(rev_008345_groups[igroup],
			   rev_008345_sockets[isocket],
			   rev_008345_data[idata], pulse_len);
}

/**
 * Emulates the PT2262 encoder used by Pollin 2605.
 * @param igroup    Socket group, valid range 0-31
 * @param isocket   Socket within the group, valid range 0-31
 * @param idata     0 = off, 1 = on
 * @return 0 on success, -1 or a negative board cause on failure
 *
 * igroup, isocket and idata are already checked by address_check()
 * before rss_send() reaches this function -- it indexes the
 * file-scope tables directly.
 */
static int32_t pollin_2605_send(uint32_t igroup, uint32_t isocket,
				uint32_t idata)
{
	const uint32_t pulse_len = 350;

	return socket_ctrl(pollin_2605_groups[igroup],
			   pollin_2605_sockets[isocket],
			   pollin_2605_data[idata], pulse_len);
}

/**
 * Assembles a tri-state codeword from its three parts and sends it.
 * @param group      Socket group code-word part
 * @param socket     Socket-within-group code-word part
 * @param data       On/off code-word part
 * @param pulse_len  Pulse length in microseconds
 * @return 0 on success, -1 or a negative board cause on failure
 */
static int32_t socket_ctrl(const char *group, const char *socket,
			   const char *data, uint32_t pulse_len)
{
	int n;
	char codeword[RSS_CODEWORD_MAX_LEN + 1];

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

	/* Send the codeword */
	return send_tris(codeword, pulse_len);
}

void rss_gpio_config(const char *config)
{
	if (pthread_mutex_lock(&send_lock) != 0) {
		syslog(LOG_ERR, "[-] pthread_mutex_lock");
		return;
	}
	gpio_config = config;
	pthread_mutex_unlock(&send_lock);
}

int32_t rss_gpio_config_check(const char *config)
{
	return board_config_check(config);
}

int32_t rss_send(uint32_t dev, uint32_t group,
		 uint32_t socket, uint32_t data)
{
	int32_t ret;

#ifdef DEBUG
	syslog(LOG_NOTICE,
	       "dev: %" PRIu32 ", group: %" PRIu32 ", socket: %" PRIu32
	       ", data: %" PRIu32, dev, group, socket, data);
#endif

	last_error = RSS_ERR_NONE;

	if (address_check(dev, group, socket, data) < 0)
		return fail(RSS_ERR_ARG);

	if (pthread_mutex_lock(&send_lock) != 0) {
		syslog(LOG_ERR, "[-] pthread_mutex_lock");
		return fail(RSS_ERR_OTHER);
	}

	ret = board_init(gpio_config);
	if (ret < 0) {
		syslog(LOG_ERR, "[-] board_init");
		pthread_mutex_unlock(&send_lock);
		return fail(cause_from_board(ret));
	}

	switch (dev) {
	case RSS_REV_008345:
		ret = rev_008345_send(group, socket, data);
		if (ret < 0)
			syslog(LOG_ERR, "[-] rev_008345_send()");
		break;
	case RSS_POLLIN_2605:
		ret = pollin_2605_send(group, socket, data);
		if (ret < 0)
			syslog(LOG_ERR, "[-] pollin_2605_send()");
		break;
	default:
		/* dev was already checked above; never reached. */
		ret = -1;
	}

	if (board_exit() < 0)
		syslog(LOG_ERR, "[-] board_exit");

	pthread_mutex_unlock(&send_lock);
	return ret < 0 ? fail(cause_from_board(ret)) : 0;
}

int32_t rss_addr_limits(uint32_t dev, uint32_t *ngroups,
			uint32_t *nsockets, uint32_t *ndata)
{
	size_t g, s, d;

	if (!ngroups || !nsockets || !ndata)
		return -1;

	if (dev_limits(dev, &g, &s, &d) < 0)
		return -1;

	*ngroups = (uint32_t)g;
	*nsockets = (uint32_t)s;
	*ndata = (uint32_t)d;
	return 0;
}

int32_t rss_send_codeword(const char *tristate, uint32_t pulse_len)
{
	size_t len;
	int32_t ret;

	last_error = RSS_ERR_NONE;

	if (!tristate) {
		syslog(LOG_ERR, "[-] Invalid tristate codeword");
		return fail(RSS_ERR_ARG);
	}

	len = strnlen(tristate, RSS_CODEWORD_MAX_LEN + 1);
	if (len < RSS_CODEWORD_MIN_LEN || len > RSS_CODEWORD_MAX_LEN) {
		syslog(LOG_ERR, "[-] Invalid tristate codeword length %zu",
		       len);
		return fail(RSS_ERR_ARG);
	}

	if (tristate_check(tristate) < 0)
		return fail(RSS_ERR_ARG);

	if (pulse_len < RSS_PULSE_LEN_MIN_US ||
	    pulse_len > RSS_PULSE_LEN_MAX_US) {
		syslog(LOG_ERR, "[-] pulse_len %" PRIu32 " out of range",
		       pulse_len);
		return fail(RSS_ERR_ARG);
	}

	if (pthread_mutex_lock(&send_lock) != 0) {
		syslog(LOG_ERR, "[-] pthread_mutex_lock");
		return fail(RSS_ERR_OTHER);
	}

	ret = board_init(gpio_config);
	if (ret < 0) {
		syslog(LOG_ERR, "[-] board_init");
		pthread_mutex_unlock(&send_lock);
		return fail(cause_from_board(ret));
	}

	ret = send_tris(tristate, pulse_len);

	if (board_exit() < 0)
		syslog(LOG_ERR, "[-] board_exit");

	pthread_mutex_unlock(&send_lock);
	return ret < 0 ? fail(cause_from_board(ret)) : 0;
}

enum rss_err rss_last_error(void)
{
	return last_error;
}
