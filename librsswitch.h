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
 * socket_send() and socket_gpio_config() (and, once added,
 * socket_send_codeword()) are safe to call concurrently from multiple
 * threads of the same process. Every send session -- board_init(),
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

enum socket_type {
		  REV_008345,
		  POLLIN_2605
};

#endif /* LIBRSSWITCH_H */
