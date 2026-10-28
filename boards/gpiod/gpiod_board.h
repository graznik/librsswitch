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
 * The default chip and line, used when no "chip:line" is configured.
 * It is line 17 of gpiochip0: GPIO17, pin 11 of the 40-pin header, on
 * the Raspberry Pi boards and, on kernel 6.x such as on Debian 13, pin
 * P9_23 of the BeagleBone Black. On other kernels the chip can differ,
 * so check it with gpioinfo and pass "chip:line" with --gpio instead.
 */
#define DEFAULT_CHIPNAME "gpiochip0"
#define DEFAULT_LINE_OFFSET 17

void delayMicrosecondsHard(unsigned int howLong);
void delayMicroseconds(unsigned int howLong);

#endif /* GPIOD_BOARD_H */
