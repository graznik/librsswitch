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
 * Exact-codeword tests (LIB-07). Asserts the tri-state codeword the
 * x86_64 board recorder captured for a socket_send() call against
 * literal expected strings, so a single wrong table entry in
 * librsswitch.c fails this suite.
 */
#include "minitest.h"
#include "../librsswitch.h"
#include "../boards/x86_64/x86_64.h"
#include "pulse_decode.h"

static void test_pollin_2605_high_address(void)
{
	x86_64_recorder_reset();
	CHECK_EQ(socket_send(POLLIN_2605, 17, 23, 1), 0);
	CHECK_EQ(expect_transmission("0FFF0000F00F", 350), 0);
}

static void test_rev_008345_entry(void)
{
	x86_64_recorder_reset();
	CHECK_EQ(socket_send(REV_008345, 2, 1, 1), 0);
	CHECK_EQ(expect_transmission("FF1FF1F00010", 350), 0);
}

static void test_decoder_is_not_vacuous(void)
{
	x86_64_recorder_reset();
	CHECK_EQ(expect_transmission("FFFFFFFFFFF0", 350), -1);

	x86_64_recorder_reset();
	CHECK_EQ(socket_send(POLLIN_2605, 0, 0, 0), 0);
	CHECK_EQ(expect_transmission("000000000000", 350), -1);
}

static void test_back_to_back_transmissions(void)
{
	char first[PULSE_DECODE_MAX_FRAME_LEN];
	char second[PULSE_DECODE_MAX_FRAME_LEN];
	uint32_t pulse_len;
	size_t pos = 0;

	x86_64_recorder_reset();
	CHECK_EQ(socket_send(POLLIN_2605, 17, 23, 1), 0);
	CHECK_EQ(socket_send(REV_008345, 2, 1, 1), 0);

	CHECK_EQ(next_transmission(&pos, first, sizeof(first), &pulse_len), 0);
	CHECK(strcmp(first, "0FFF0000F00F") == 0);
	CHECK_EQ(next_transmission(&pos, second, sizeof(second), &pulse_len), 0);
	CHECK(strcmp(second, "FF1FF1F00010") == 0);
	CHECK_EQ(pos, x86_64_recorder_count());
}

int main(void)
{
	test_pollin_2605_high_address();
	test_rev_008345_entry();
	test_decoder_is_not_vacuous();
	test_back_to_back_transmissions();

	return MT_REPORT();
}
