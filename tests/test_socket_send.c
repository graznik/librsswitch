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
 * devices. Built against the x86_64 stub board, so this runs on any
 * host, no hardware needed. Codeword content is checked in
 * test_codewords.c; this file covers the return-code and
 * no-transmission contract.
 */
#include "minitest.h"
#include "../librsswitch.h"
#include "../boards/x86_64/x86_64.h"
#include "pulse_decode.h"

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

static void test_rev_008345_boundary(void)
{
	x86_64_recorder_reset();
	CHECK_EQ(socket_send(REV_008345, 3, 2, 0), 0);
	CHECK_EQ(expect_transmission("FFF1FF100001", 350), 0);
}

static void test_pollin_2605_boundary(void)
{
	x86_64_recorder_reset();
	CHECK_EQ(socket_send(POLLIN_2605, 31, 31, 0), 0);
	CHECK_EQ(expect_transmission("0000000000F0", 350), 0);
}

/*
 * A rejected address must not leave a partial transmission on the
 * air: every out-of-range call the tests above already reject must
 * record zero pulses.
 */
static void test_rejected_sends_transmit_nothing(void)
{
	x86_64_recorder_reset();
	CHECK_EQ(socket_send(REV_008345, 4, 0, 0), -1);   /* group */
	CHECK_EQ(x86_64_recorder_count(), (size_t)0);

	x86_64_recorder_reset();
	CHECK_EQ(socket_send(REV_008345, 0, 3, 0), -1);   /* socket */
	CHECK_EQ(x86_64_recorder_count(), (size_t)0);

	x86_64_recorder_reset();
	CHECK_EQ(socket_send(REV_008345, 0, 0, 2), -1);   /* data */
	CHECK_EQ(x86_64_recorder_count(), (size_t)0);

	x86_64_recorder_reset();
	CHECK_EQ(socket_send(POLLIN_2605, 32, 0, 0), -1);  /* group */
	CHECK_EQ(x86_64_recorder_count(), (size_t)0);

	x86_64_recorder_reset();
	CHECK_EQ(socket_send(POLLIN_2605, 0, 32, 0), -1);  /* socket */
	CHECK_EQ(x86_64_recorder_count(), (size_t)0);

	x86_64_recorder_reset();
	CHECK_EQ(socket_send(POLLIN_2605, 0, 0, 2), -1);   /* data */
	CHECK_EQ(x86_64_recorder_count(), (size_t)0);

	x86_64_recorder_reset();
	CHECK_EQ(socket_send(99, 0, 0, 0), -1);            /* device */
	CHECK_EQ(x86_64_recorder_count(), (size_t)0);
}

int main(void)
{
	test_rev_008345_valid();
	test_rev_008345_out_of_range();
	test_rev_008345_boundary();
	test_pollin_2605_valid();
	test_pollin_2605_out_of_range();
	test_pollin_2605_boundary();
	test_unknown_device();
	test_rejected_sends_transmit_nothing();

	return MT_REPORT();
}
