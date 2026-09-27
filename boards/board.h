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
 * Adding a new platform: implement these three functions in a new
 * boards/<platform>/<platform>.c + .h, add a Makefile target. Nothing
 * outside boards/<platform>/ needs to change.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

/*
 * config: board-specific, opaque to callers outside this board's own
 * .c file. NULL means "use this board's compiled-in default". Each
 * board documents and parses its own format -- see the board_init()
 * doc comment in the matching boards/<platform>/<platform>.c.
 *
 * board_init() opens a send session with the line driven low, even if
 * an earlier process left it high (G-01-5) -- so a stuck-high line is
 * bounded to "until the next board_init()" regardless of how it got
 * that way. The x86_64 stub has no real line, so it has nothing to
 * drive; its board_init()/board_exit()/transmit() are no-ops.
 */
int32_t board_init(const char *config);

/*
 * Ends the send session opened by board_init(), closing or releasing
 * the line. If the line may still be high (a transmit() failed after
 * driving it high), board_exit() first drives it low, so no session
 * ends with the 433 MHz transmitter keyed (G-01-5). A failure to
 * drive it low is logged and makes board_exit() return -1, but the
 * line is closed or released regardless.
 */
int32_t board_exit(void);

/*
 * Drives one high/low pulse pair. Returns 0 on success, -1 on error,
 * in which case the caller abandons the rest of the codeword (no
 * further transmit() call is made for that send) -- the line may be
 * left high by such a failure, and the caller's board_exit() then
 * drives it low again.
 */
int32_t transmit(uint32_t nhigh, uint32_t nlow, uint32_t pulse_len);

#endif /* BOARD_H */
