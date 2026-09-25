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
 */
int32_t board_init(const char *config);
int32_t board_exit(void);
void transmit(uint32_t nhigh, uint32_t nlow, uint32_t pulse_len);

#endif /* BOARD_H */
