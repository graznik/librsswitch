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

/*
 * Shared contract every boards/<platform>/<platform>.c must implement.
 * Declared once here so the compiler checks each board against the
 * same signatures, instead of each board header re-declaring them by
 * convention (a mismatch here was previously only caught by chance).
 *
 * Adding a new platform: implement these four functions in a new
 * boards/<platform>/<platform>.c + .h, add a Makefile target. Nothing
 * outside boards/<platform>/ needs to change. A board that drives its
 * line through libgpiod implements none of these functions itself: it
 * defines its default chip and line in boards/<platform>/<platform>.h,
 * and its Makefile target builds boards/gpiod/gpiod_board.c.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

/* The causes a board returns are the SOCKET_ERR_ values declared there. */
#include "../librsswitch.h"

/*
 * A board reports a failure with a negative value. -1 means "other"
 * (-(int32_t)RSS_ERR_OTHER); a board that can name the cause returns
 * -(int32_t)SOCKET_ERR_x for it instead. A board never returns
 * RSS_ERR_NONE or RSS_ERR_ARG as a negative value, and the
 * library reads any other negative value as RSS_ERR_OTHER. Each
 * function below says which causes it can return.
 *
 * config: board-specific, opaque to the library's callers. NULL means
 * "use this board's compiled-in default". Each board documents and
 * parses its own format. The libgpiod boards share the "chip:line"
 * format documented in boards/gpiod/gpiod_config.h. On those boards a
 * non-NULL string that cannot be parsed makes board_init() return -1
 * without opening the GPIO; it never selects the default instead. The
 * x86_64 stub applies the same format in board_config_check(), but its
 * board_init() drives nothing and accepts any string, except a chip
 * named "fail" plus a cause word (see boards/x86_64/x86_64.c), which
 * makes it fail with that cause so tests reach every failure path.
 *
 * board_init() opens a send session with the line driven low, even if
 * an earlier process left it high -- so a stuck-high line is
 * bounded to "until the next board_init()" regardless of how it got
 * that way. The x86_64 stub has no real line, so it has nothing to
 * drive; its board_init()/board_exit()/transmit() are no-ops.
 *
 * Returns 0, or -(int32_t)SOCKET_ERR_x for a failure the board can
 * name (RSS_ERR_NOCHIP, RSS_ERR_BUSY, RSS_ERR_DENIED or
 * RSS_ERR_BADLINE), or -1 for any other failure.
 */
int32_t board_init(const char *config);

/*
 * Reports whether board_init() could use config, without opening,
 * storing or changing anything. Returns 0 for NULL and for a
 * well-formed config, -1 for a malformed one, after the cause has been
 * logged via syslog. It checks the format only, so a well-formed
 * config may still name a chip that is missing; the first
 * board_init() reports that. Callable at any time from any thread.
 */
int32_t board_config_check(const char *config);

/*
 * Ends the send session opened by board_init(), closing or releasing
 * the line. If the line may still be high (a transmit() failed after
 * driving it high), board_exit() first drives it low, so no session
 * ends with the 433 MHz transmitter keyed. A failure to
 * drive it low is logged and makes board_exit() return
 * -(int32_t)RSS_ERR_IO, but the line is closed or released
 * regardless. Returns 0 otherwise.
 */
int32_t board_exit(void);

/*
 * Drives one high/low pulse pair. Returns 0 on success, or a negative
 * cause on error -- normally -(int32_t)RSS_ERR_IO, or -1 for a
 * failure the board cannot name -- in which case the caller abandons
 * the rest of the codeword (no further transmit() call is made for
 * that send) -- the line may be left high by such a failure, and the
 * caller's board_exit() then drives it low again.
 */
int32_t transmit(uint32_t nhigh, uint32_t nlow, uint32_t pulse_len);

#endif /* BOARD_H */
