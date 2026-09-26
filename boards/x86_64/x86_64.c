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

static struct x86_64_pulse recorder[X86_64_RECORDER_MAX_PULSES];
static size_t recorder_count;
static int recorder_overflow;
static size_t init_count;
static size_t exit_count;

int32_t board_init(const char *config)
{
	(void)config;
	init_count++;
	return 0;
}

int32_t board_exit(void)
{
	exit_count++;
	return 0;
}

int32_t transmit(uint32_t nhigh, uint32_t nlow, uint32_t pulse_len)
{
	if (recorder_count >= X86_64_RECORDER_MAX_PULSES) {
		recorder_overflow = 1;
		return 0;
	}

	recorder[recorder_count].nhigh = nhigh;
	recorder[recorder_count].nlow = nlow;
	recorder[recorder_count].pulse_len = pulse_len;
	recorder_count++;
	return 0;
}

void x86_64_recorder_reset(void)
{
	recorder_count = 0;
	recorder_overflow = 0;
	init_count = 0;
	exit_count = 0;
}

size_t x86_64_recorder_count(void)
{
	return recorder_count;
}

const struct x86_64_pulse *x86_64_recorder_get(size_t index)
{
	if (index >= recorder_count)
		return NULL;

	return &recorder[index];
}

int x86_64_recorder_overflowed(void)
{
	return recorder_overflow;
}

size_t x86_64_board_init_count(void)
{
	return init_count;
}

size_t x86_64_board_exit_count(void)
{
	return exit_count;
}
