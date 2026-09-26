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

#include "x86_64.h"

int32_t board_init(const char *config)
{
	(void)config;
	return 0;
}

int32_t board_exit(void)
{
	return 0;
}

void transmit(uint32_t nhigh, uint32_t nlow, uint32_t pulse_len)
{
	(void)nhigh;
	(void)nlow;
	(void)pulse_len;
}
