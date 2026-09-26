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
 * librsswitch.c fails this suite. The expected-value tables below are
 * written independently of librsswitch.c's own lookup tables -- they
 * do not reference them in any way -- so corrupting any one entry in
 * either file is caught.
 */
#include "minitest.h"
#include "../librsswitch.h"
#include "../boards/x86_64/x86_64.h"
#include "pulse_decode.h"

static const char * const rev_groups[4] = {"1FFF", "F1FF", "FF1F", "FFF1"};
static const char * const rev_sockets[3] = {"1FF0", "F1F0", "FF10"};
static const char * const rev_data[2] = {"0001", "0010"};

static const char * const pollin_addr[32] = {
	"FFFFF", "0FFFF", "F0FFF", "00FFF",
	"FF0FF", "0F0FF", "F00FF", "000FF",
	"FFF0F", "0FF0F", "F0F0F", "00F0F",
	"FF00F", "0F00F", "F000F", "0000F",
	"FFFF0", "0FFF0", "F0FF0", "00FF0",
	"FF0F0", "0F0F0", "F00F0", "000F0",
	"FFF00", "0FF00", "F0F00", "00F00",
	"FF000", "0F000", "F0000", "00000",
};
static const char * const pollin_data[2] = {"F0", "0F"}; /* Off, On */

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

static void test_rev_008345_all_entries(void)
{
	uint32_t group, socket, data;

	for (group = 0; group < 4; group++) {
		for (socket = 0; socket < 3; socket++) {
			for (data = 0; data < 2; data++) {
				char expected[16];

				x86_64_recorder_reset();
				CHECK_EQ(socket_send(REV_008345, group, socket,
						      data), 0);
				snprintf(expected, sizeof(expected), "%s%s%s",
					 rev_groups[group], rev_sockets[socket],
					 rev_data[data]);
				CHECK_EQ(expect_transmission(expected, 350), 0);
			}
		}
	}
}

static void test_pollin_2605_all_entries(void)
{
	uint32_t group, socket, data;

	for (group = 0; group < 32; group++) {
		for (socket = 0; socket < 32; socket++) {
			for (data = 0; data < 2; data++) {
				char expected[16];

				x86_64_recorder_reset();
				CHECK_EQ(socket_send(POLLIN_2605, group, socket,
						      data), 0);
				snprintf(expected, sizeof(expected), "%s%s%s",
					 pollin_addr[group], pollin_addr[socket],
					 pollin_data[data]);
				CHECK_EQ(expect_transmission(expected, 350), 0);
			}
		}
	}
}

/*
 * pollin_addr[n] must be n written as 5 bits, LSB first, with a set
 * bit as '0' and a clear bit as 'F' (the Tri-State encoding PT2262
 * uses). rev_groups[g]/rev_sockets[s] must each hold exactly one '1'
 * character, located at index g/s -- these self-checks guard against
 * a typo in this file's own literal tables above, independently of
 * whether librsswitch.c's tables are correct.
 */
static int pollin_addr_matches(const char *s, uint32_t n)
{
	size_t i;

	for (i = 0; i < 5; i++) {
		int bit = (n >> i) & 1;
		char expected = bit ? '0' : 'F';

		if (s[i] != expected)
			return 0;
	}

	return s[5] == '\0';
}

static int one_hot_at(const char *s, size_t idx)
{
	size_t i;
	size_t ones = 0;

	for (i = 0; s[i] != '\0'; i++) {
		if (s[i] == '1') {
			ones++;
			if (i != idx)
				return 0;
		}
	}

	return ones == 1;
}

static void test_expected_tables_self_check(void)
{
	uint32_t n, group, socket;

	for (n = 0; n < 32; n++)
		CHECK(pollin_addr_matches(pollin_addr[n], n));

	for (group = 0; group < 4; group++)
		CHECK(one_hot_at(rev_groups[group], group));
	for (socket = 0; socket < 3; socket++)
		CHECK(one_hot_at(rev_sockets[socket], socket));
}

int main(void)
{
	test_pollin_2605_high_address();
	test_rev_008345_entry();
	test_decoder_is_not_vacuous();
	test_back_to_back_transmissions();
	test_expected_tables_self_check();
	test_rev_008345_all_entries();
	test_pollin_2605_all_entries();

	return MT_REPORT();
}
