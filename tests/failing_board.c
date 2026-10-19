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
 * Test-only board whose board_init() always fails. Linked into
 * test_rsswitchd_failbin instead of a real board, so a socket-level
 * test can reach the path where socket_send() fails after the
 * daemon's own checks already passed: a well-formed,
 * in-range command still reaches the library, and the library's
 * board_init() call is what fails, exactly as a real GPIO failure
 * would. Its config passes the check, so only board_init() fails.
 */
#include "../boards/board.h"

int32_t board_init(const char *config)
{
	(void)config;
	return -1;
}

int32_t board_config_check(const char *config)
{
	(void)config;
	return 0;
}

int32_t board_exit(void)
{
	return 0;
}

int32_t transmit(uint32_t nhigh, uint32_t nlow, uint32_t pulse_len)
{
	(void)nhigh;
	(void)nlow;
	(void)pulse_len;
	return -1;
}
