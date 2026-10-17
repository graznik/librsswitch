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

#ifndef GPIOD_BOARD_H
#define GPIOD_BOARD_H

#include "../board.h"

/*
 * Each libgpiod board provides its defaults with its own prefix
 * (RPI_DEFAULT_*, ...) in its header, so both headers can be read side
 * by side; the board macro picks the ones used here.
 */
#if defined(RASPBERRY_PI)
#include "../rpi/rpi.h"
#define DEFAULT_CHIPNAME RPI_DEFAULT_CHIPNAME
#define DEFAULT_LINE_OFFSET RPI_DEFAULT_LINE_OFFSET
#else
#error "boards/gpiod/gpiod_board.c needs a libgpiod board macro (-DRASPBERRY_PI)"
#endif

void delayMicrosecondsHard(unsigned int howLong);
void delayMicroseconds(unsigned int howLong);

#endif /* GPIOD_BOARD_H */
