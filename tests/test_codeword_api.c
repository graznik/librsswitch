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
 * Tests send_tris()'s strict alphabet enforcement (LIB-09) and the
 * raw tri-state API socket_send_codeword() (LIB-04). Includes
 * ../librsswitch.c directly, instead of linking it, so this file can
 * call the static send_tris() and socket_ctrl() directly -- neither
 * is part of the public API in librsswitch.h. This binary therefore
 * links only ../boards/x86_64/x86_64.c, not librsswitch.c a second
 * time.
 */
#include "minitest.h"
#include "pulse_decode.h"
#include "../librsswitch.c"

static void test_send_tris_rejects_invalid(void)
{
	static const char * const bad[] = {
		"01X0", "0f", "0 1", "1\n", "\xc3\x9f",
	};
	size_t i;

	x86_64_recorder_reset();
	CHECK_EQ(send_tris(NULL, 350), -1);
	CHECK_EQ(x86_64_recorder_count(), (size_t)0);

	x86_64_recorder_reset();
	CHECK_EQ(send_tris("", 350), -1);
	CHECK_EQ(x86_64_recorder_count(), (size_t)0);

	for (i = 0; i < ARRAY_SIZE(bad); i++) {
		x86_64_recorder_reset();
		CHECK_EQ(send_tris(bad[i], 350), -1);
		CHECK_EQ(x86_64_recorder_count(), (size_t)0);
	}
}

static void test_send_tris_accepts_valid(void)
{
	x86_64_recorder_reset();
	CHECK_EQ(send_tris("0F1", 350), 0);
	CHECK_EQ(expect_transmission("0F1", 350), 0);

	x86_64_recorder_reset();
	CHECK_EQ(send_tris("F", 350), 0);
	CHECK_EQ(x86_64_recorder_count(), (size_t)30);
}

static void test_socket_ctrl_propagates_rejection(void)
{
	x86_64_recorder_reset();
	CHECK_EQ(socket_ctrl("1FFF", "1FX0", "0001", 350), -1);
	CHECK_EQ(x86_64_recorder_count(), (size_t)0);
}

int main(void)
{
	test_send_tris_rejects_invalid();
	test_send_tris_accepts_valid();
	test_socket_ctrl_propagates_rejection();

	return MT_REPORT();
}
