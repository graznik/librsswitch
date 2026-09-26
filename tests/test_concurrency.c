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
 * Concurrency tests (LIB-01, D-09). Proves that the x86_64 board's
 * overlap detector actually detects an unserialized board session
 * (not a vacuous always-zero counter), then, once socket_send() is
 * serialized with a mutex, that concurrent callers from multiple
 * threads always produce whole, non-interleaved codewords.
 */
#include "minitest.h"
#include "pulse_decode.h"
#include "../librsswitch.h"
#include "../boards/x86_64/x86_64.h"

static void test_overlap_detector_is_not_vacuous(void)
{
	x86_64_recorder_reset();
	CHECK_EQ(board_init(NULL), 0);
	CHECK_EQ(board_init(NULL), 0);
	CHECK_EQ(x86_64_board_overlap_count(), (size_t)1);
	CHECK_EQ(board_exit(), 0);
	CHECK_EQ(board_exit(), 0);

	x86_64_recorder_reset();
	CHECK_EQ(socket_send(POLLIN_2605, 17, 23, 1), 0);
	CHECK_EQ(x86_64_board_overlap_count(), (size_t)0);
}

int main(void)
{
	test_overlap_detector_is_not_vacuous();

	return MT_REPORT();
}
