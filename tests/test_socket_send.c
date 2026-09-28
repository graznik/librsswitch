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

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

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
 * air, and must not open a board session at all: every out-of-range
 * call the tests above already reject must record zero pulses and
 * make zero board_init()/board_exit() calls. The final control send
 * shows exactly one of each, so the counters are not vacuous.
 */
static void test_rejected_sends_transmit_nothing(void)
{
	x86_64_recorder_reset();
	CHECK_EQ(socket_send(REV_008345, 4, 0, 0), -1);   /* group */
	CHECK_EQ(x86_64_recorder_count(), (size_t)0);
	CHECK_EQ(x86_64_board_init_count(), (size_t)0);
	CHECK_EQ(x86_64_board_exit_count(), (size_t)0);

	x86_64_recorder_reset();
	CHECK_EQ(socket_send(REV_008345, 0, 3, 0), -1);   /* socket */
	CHECK_EQ(x86_64_recorder_count(), (size_t)0);
	CHECK_EQ(x86_64_board_init_count(), (size_t)0);
	CHECK_EQ(x86_64_board_exit_count(), (size_t)0);

	x86_64_recorder_reset();
	CHECK_EQ(socket_send(REV_008345, 0, 0, 2), -1);   /* data */
	CHECK_EQ(x86_64_recorder_count(), (size_t)0);
	CHECK_EQ(x86_64_board_init_count(), (size_t)0);
	CHECK_EQ(x86_64_board_exit_count(), (size_t)0);

	x86_64_recorder_reset();
	CHECK_EQ(socket_send(POLLIN_2605, 32, 0, 0), -1);  /* group */
	CHECK_EQ(x86_64_recorder_count(), (size_t)0);
	CHECK_EQ(x86_64_board_init_count(), (size_t)0);
	CHECK_EQ(x86_64_board_exit_count(), (size_t)0);

	x86_64_recorder_reset();
	CHECK_EQ(socket_send(POLLIN_2605, 0, 32, 0), -1);  /* socket */
	CHECK_EQ(x86_64_recorder_count(), (size_t)0);
	CHECK_EQ(x86_64_board_init_count(), (size_t)0);
	CHECK_EQ(x86_64_board_exit_count(), (size_t)0);

	x86_64_recorder_reset();
	CHECK_EQ(socket_send(POLLIN_2605, 0, 0, 2), -1);   /* data */
	CHECK_EQ(x86_64_recorder_count(), (size_t)0);
	CHECK_EQ(x86_64_board_init_count(), (size_t)0);
	CHECK_EQ(x86_64_board_exit_count(), (size_t)0);

	x86_64_recorder_reset();
	CHECK_EQ(socket_send(99, 0, 0, 0), -1);            /* device */
	CHECK_EQ(x86_64_recorder_count(), (size_t)0);
	CHECK_EQ(x86_64_board_init_count(), (size_t)0);
	CHECK_EQ(x86_64_board_exit_count(), (size_t)0);

	/* Control: a valid send still opens exactly one board session. */
	x86_64_recorder_reset();
	CHECK_EQ(socket_send(POLLIN_2605, 0, 0, 0), 0);
	CHECK_EQ(x86_64_board_init_count(), (size_t)1);
	CHECK_EQ(x86_64_board_exit_count(), (size_t)1);
}

/*
 * A failing transmit() must abort the rest of the codeword: no pulse
 * after the failing one is recorded, board_exit() still runs exactly
 * once, and socket_send() returns -1. Exercised at four points across
 * a 12-symbol, 10-repetition POLLIN_2605 codeword: the very first
 * pulse, inside a symbol, at the first sync pulse, and inside the
 * second frame.
 */
static void test_transmit_failure_aborts_codeword(void)
{
	static const size_t fail_points[] = {0, 5, 24, 30};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(fail_points); i++) {
		size_t idx = fail_points[i];

		x86_64_recorder_reset();
		x86_64_recorder_fail_at(idx);
		CHECK_EQ(socket_send(POLLIN_2605, 17, 23, 1), -1);
		CHECK_EQ(x86_64_recorder_count(), idx);
		CHECK_EQ(x86_64_board_exit_count(), (size_t)1);
	}

	x86_64_recorder_reset();
	CHECK_EQ(socket_send(POLLIN_2605, 17, 23, 1), 0);
	CHECK_EQ(expect_transmission("0FFF0000F00F", 350), 0);
}

/*
 * socket_addr_limits() reports the same bounds address_check()
 * enforces, for both devices; rejects an unknown dev or any NULL
 * pointer without touching its outputs; and every address at or one
 * past each device's reported limit gets the same accept/reject
 * verdict from socket_send() -- with no board session opened for a
 * rejected address (D-13).
 */
static void test_addr_limits(void)
{
	uint32_t ngroups, nsockets, ndata;
	uint32_t sg, ss, sd;
	static const struct {
		enum socket_type dev;
		uint32_t ngroups, nsockets, ndata;
	} devs[] = {
		{ REV_008345, 4, 3, 2 },
		{ POLLIN_2605, 32, 32, 2 },
	};
	size_t i;

	CHECK_EQ(socket_addr_limits(REV_008345, &ngroups, &nsockets, &ndata),
		 0);
	CHECK_EQ(ngroups, (uint32_t)4);
	CHECK_EQ(nsockets, (uint32_t)3);
	CHECK_EQ(ndata, (uint32_t)2);

	CHECK_EQ(socket_addr_limits(POLLIN_2605, &ngroups, &nsockets, &ndata),
		 0);
	CHECK_EQ(ngroups, (uint32_t)32);
	CHECK_EQ(nsockets, (uint32_t)32);
	CHECK_EQ(ndata, (uint32_t)2);

	sg = ss = sd = 999;
	CHECK_EQ(socket_addr_limits(2, &sg, &ss, &sd), -1);
	CHECK_EQ(sg, (uint32_t)999);
	CHECK_EQ(ss, (uint32_t)999);
	CHECK_EQ(sd, (uint32_t)999);

	CHECK_EQ(socket_addr_limits(REV_008345, NULL, &nsockets, &ndata), -1);
	CHECK_EQ(socket_addr_limits(REV_008345, &ngroups, NULL, &ndata), -1);
	CHECK_EQ(socket_addr_limits(REV_008345, &ngroups, &nsockets, NULL),
		 -1);

	for (i = 0; i < ARRAY_SIZE(devs); i++) {
		x86_64_recorder_reset();
		CHECK_EQ(socket_send(devs[i].dev, devs[i].ngroups - 1,
				      devs[i].nsockets - 1,
				      devs[i].ndata - 1), 0);
		CHECK_EQ(x86_64_board_init_count(), (size_t)1);

		x86_64_recorder_reset();
		CHECK_EQ(socket_send(devs[i].dev, devs[i].ngroups,
				      devs[i].nsockets - 1,
				      devs[i].ndata - 1), -1);
		CHECK_EQ(x86_64_board_init_count(), (size_t)0);

		x86_64_recorder_reset();
		CHECK_EQ(socket_send(devs[i].dev, devs[i].ngroups - 1,
				      devs[i].nsockets,
				      devs[i].ndata - 1), -1);
		CHECK_EQ(x86_64_board_init_count(), (size_t)0);

		x86_64_recorder_reset();
		CHECK_EQ(socket_send(devs[i].dev, devs[i].ngroups - 1,
				      devs[i].nsockets - 1,
				      devs[i].ndata), -1);
		CHECK_EQ(x86_64_board_init_count(), (size_t)0);
	}
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
	test_transmit_failure_aborts_codeword();
	test_addr_limits();

	return MT_REPORT();
}
