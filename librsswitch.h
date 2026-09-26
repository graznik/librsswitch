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

#ifndef LIBRSSWITCH_H
#define LIBRSSWITCH_H

#include <stdint.h>

/*
 * Thread safety
 *
 * socket_send(), socket_send_codeword() and socket_gpio_config() are
 * safe to call concurrently from multiple threads of the same
 * process. Every send session -- board_init(),
 * all repetitions, board_exit() -- is serialized by one library-wide
 * lock, so concurrent sends never interleave their pulses; a calling
 * thread simply blocks until the previous send session completes.
 * socket_gpio_config() takes the same lock around its store, so a
 * config change is ordered against sends that start after it returns
 * (it does not affect a session already in progress).
 *
 * This guarantee is process-local only: two separate processes
 * driving the same GPIO line (for example rsssend run once while
 * rsswitchd is active) are not serialized against each other.
 *
 * The library is not async-signal-safe -- never call any of these
 * functions from a signal handler. Cancelling a thread while it is
 * inside a send is not supported; doing so may leave the lock held.
 */

/*
 * Sets the GPIO config string passed to board_init() on every
 * subsequent socket_send() call. NULL (the default, if never called)
 * means "use this board's compiled-in default". Format is board-
 * specific -- see boards/<platform>/<platform>.c's board_init() doc
 * comment. config is not copied; the pointer must stay valid for as
 * long as socket_send() may still be called.
 */
void socket_gpio_config(const char *config);

int32_t socket_send(uint32_t dev, uint32_t group,
		    uint32_t socket, uint32_t data);

/*
 * Bounds for socket_send_codeword()'s tristate argument: byte length
 * before the first NUL, in [SOCKET_CODEWORD_MIN_LEN,
 * SOCKET_CODEWORD_MAX_LEN]. 12 is the PT2260/PT2262 tri-state word
 * length -- both supported chips use a 12-symbol codeword.
 */
#define SOCKET_CODEWORD_MIN_LEN 1U
#define SOCKET_CODEWORD_MAX_LEN 12U

/*
 * Bounds for socket_send_codeword()'s pulse_len argument, in
 * microseconds. Wide enough for both supported chips at their
 * documented 350us pulse, narrow enough that a typo cannot hold the
 * GPIO line high for more than a few milliseconds per pulse.
 */
#define SOCKET_PULSE_LEN_MIN_US 50U
#define SOCKET_PULSE_LEN_MAX_US 2000U

/*
 * Sends a raw tri-state codeword, instead of one of the two built-in
 * device tables socket_send() encodes.
 *
 * tristate's length (the number of bytes before its first NUL) must
 * be within [SOCKET_CODEWORD_MIN_LEN, SOCKET_CODEWORD_MAX_LEN]. Every
 * one of those bytes must be the ASCII character '0', '1' or
 * uppercase 'F' -- lowercase, whitespace, and any other byte
 * (including a multi-byte UTF-8 sequence) is rejected. tristate is
 * never modified or truncated: it is sent exactly as given, or
 * rejected in full.
 *
 * pulse_len is in microseconds and must be within
 * [SOCKET_PULSE_LEN_MIN_US, SOCKET_PULSE_LEN_MAX_US].
 *
 * On success, tristate is sent NTIMES (10 -- the same repeat count
 * socket_send() uses), each repetition followed by a sync pulse.
 * Input is fully validated before board_init() or any GPIO access;
 * an invalid tristate or pulse_len never touches the board. On the
 * x86_64 build, the codeword is printed to stdout, exactly like
 * socket_send() does.
 *
 * Returns 0 on success, -1 on error (invalid input, or a board
 * error). Serialized with socket_send() by the same lock -- see
 * "Thread safety" above.
 */
int32_t socket_send_codeword(const char *tristate, uint32_t pulse_len);

enum socket_type {
		  REV_008345,
		  POLLIN_2605
};

#endif /* LIBRSSWITCH_H */
