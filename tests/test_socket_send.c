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
 * Tests socket_send()'s return-code contract for both supported
 * devices. Built against the x86_64 stub board (transmit()/board_init()/
 * board_exit() are no-ops), so this runs on any host, no hardware
 * needed. Does not check the generated codeword content -- socket_ctrl()
 * only exposes that via an X86_64-only debug fprintf, not a return
 * value; capturing/parsing stdout for that is a reasonable next step,
 * left out here to keep this first pass focused on the boundary checks.
 */
#include "minitest.h"
#include "../librsswitch.h"

static void test_rev_008345_valid(void)
{
	uint32_t group, socket, data;

	for (group = 0; group < 4; group++)
		for (socket = 0; socket < 3; socket++)
			for (data = 0; data < 2; data++)
				CHECK_EQ(socket_send(REV_008345, group, socket, data), 0);
}

static void test_rev_008345_out_of_range(void)
{
	CHECK_EQ(socket_send(REV_008345, 4, 0, 0), -1);   /* group */
	CHECK_EQ(socket_send(REV_008345, 0, 3, 0), -1);   /* socket */
	CHECK_EQ(socket_send(REV_008345, 0, 0, 2), -1);   /* data */
}

static void test_pollin_2605_valid(void)
{
	CHECK_EQ(socket_send(POLLIN_2605, 0, 0, 0), 0);
	CHECK_EQ(socket_send(POLLIN_2605, 31, 31, 1), 0);
	CHECK_EQ(socket_send(POLLIN_2605, 15, 15, 0), 0);
}

static void test_pollin_2605_out_of_range(void)
{
	CHECK_EQ(socket_send(POLLIN_2605, 32, 0, 0), -1);  /* group */
	CHECK_EQ(socket_send(POLLIN_2605, 0, 32, 0), -1);  /* socket */
	CHECK_EQ(socket_send(POLLIN_2605, 0, 0, 2), -1);   /* data */
}

static void test_unknown_device(void)
{
	CHECK_EQ(socket_send(99, 0, 0, 0), -1);
}

int main(void)
{
	test_rev_008345_valid();
	test_rev_008345_out_of_range();
	test_pollin_2605_valid();
	test_pollin_2605_out_of_range();
	test_unknown_device();

	return MT_REPORT();
}
