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
 * Tests send_tris()'s strict alphabet enforcement and the
 * raw tri-state API socket_send_codeword(). Includes
 * ../librsswitch.c directly, instead of linking it, so this file can
 * call the static send_tris() and socket_ctrl() directly -- neither
 * is part of the public API in librsswitch.h. This binary therefore
 * links only ../boards/x86_64/x86_64.c, not librsswitch.c a second
 * time.
 */
#include <stdint.h>
#include <unistd.h>

#include "minitest.h"
#include "pulse_decode.h"
#include "../librsswitch.c"

/*
 * Redirects stdout to a scratch tmpfile for the duration of a
 * capture, so a test can assert exactly what socket_send_codeword()
 * did or did not print without polluting the test binary's own
 * stdout.
 */
static void capture_stdout_begin(int *saved_fd, FILE **tmp)
{
	fflush(stdout);
	*saved_fd = dup(fileno(stdout));
	*tmp = tmpfile();
	dup2(fileno(*tmp), fileno(stdout));
}

static void capture_stdout_end(int saved_fd, FILE *tmp, char *buf,
				size_t buflen)
{
	size_t n;

	fflush(stdout);
	dup2(saved_fd, fileno(stdout));
	close(saved_fd);

	rewind(tmp);
	n = fread(buf, 1, buflen - 1, tmp);
	buf[n] = '\0';
	fclose(tmp);
}

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

static void test_codeword_api_valid(void)
{
	x86_64_recorder_reset();
	CHECK_EQ(socket_send_codeword("1", 350), 0);
	CHECK_EQ(x86_64_recorder_count(), (size_t)30);

	x86_64_recorder_reset();
	CHECK_EQ(socket_send_codeword("000000000000", 350), 0);
	CHECK_EQ(expect_transmission("000000000000", 350), 0);

	/* Embedded NUL: only "01" (before it) is sent. */
	x86_64_recorder_reset();
	CHECK_EQ(socket_send_codeword("01\0F", 350), 0);
	CHECK_EQ(expect_transmission("01", 350), 0);
}

static void test_codeword_api_rejects(void)
{
	static const char * const bad[] = {
		"", "0000000000000", "0f00", " 0", "0\n", "\xc3\x9f",
		"0\xef\xbc\xa6",
	};
	int saved_fd;
	FILE *tmp;
	char buf[64];
	size_t i;

	x86_64_recorder_reset();
	capture_stdout_begin(&saved_fd, &tmp);

	CHECK_EQ(socket_send_codeword(NULL, 350), -1);
	for (i = 0; i < ARRAY_SIZE(bad); i++)
		CHECK_EQ(socket_send_codeword(bad[i], 350), -1);

	capture_stdout_end(saved_fd, tmp, buf, sizeof(buf));
	CHECK_EQ(buf[0], '\0');

	CHECK_EQ(x86_64_recorder_count(), (size_t)0);
	CHECK_EQ(x86_64_board_init_count(), (size_t)0);
}

static void test_codeword_api_bounds(void)
{
	x86_64_recorder_reset();
	CHECK_EQ(socket_send_codeword("1", SOCKET_PULSE_LEN_MIN_US), 0);
	CHECK_EQ(expect_transmission("1", SOCKET_PULSE_LEN_MIN_US), 0);

	x86_64_recorder_reset();
	CHECK_EQ(socket_send_codeword("1", SOCKET_PULSE_LEN_MAX_US), 0);
	CHECK_EQ(expect_transmission("1", SOCKET_PULSE_LEN_MAX_US), 0);

	x86_64_recorder_reset();
	CHECK_EQ(socket_send_codeword("1", (uint32_t)0), -1);
	CHECK_EQ(x86_64_board_init_count(), (size_t)0);

	x86_64_recorder_reset();
	CHECK_EQ(socket_send_codeword("1", SOCKET_PULSE_LEN_MIN_US - 1), -1);
	CHECK_EQ(x86_64_board_init_count(), (size_t)0);

	x86_64_recorder_reset();
	CHECK_EQ(socket_send_codeword("1", SOCKET_PULSE_LEN_MAX_US + 1), -1);
	CHECK_EQ(x86_64_board_init_count(), (size_t)0);

	x86_64_recorder_reset();
	CHECK_EQ(socket_send_codeword("1", UINT32_MAX), -1);
	CHECK_EQ(x86_64_board_init_count(), (size_t)0);
}

static void test_codeword_api_matches_socket_send(void)
{
	x86_64_recorder_reset();
	CHECK_EQ(socket_send(POLLIN_2605, 17, 23, 1), 0);
	CHECK_EQ(expect_transmission("0FFF0000F00F", 350), 0);

	x86_64_recorder_reset();
	CHECK_EQ(socket_send_codeword("0FFF0000F00F", 350), 0);
	CHECK_EQ(expect_transmission("0FFF0000F00F", 350), 0);
}

static void test_codeword_api_prints_codeword(void)
{
	int saved_fd;
	FILE *tmp;
	char buf[64];

	x86_64_recorder_reset();
	capture_stdout_begin(&saved_fd, &tmp);
	CHECK_EQ(socket_send_codeword("0FFF0000F00F", 350), 0);
	capture_stdout_end(saved_fd, tmp, buf, sizeof(buf));
	CHECK(strcmp(buf, "0FFF0000F00F\n") == 0);
}

int main(void)
{
	test_send_tris_rejects_invalid();
	test_send_tris_accepts_valid();
	test_socket_ctrl_propagates_rejection();
	test_codeword_api_valid();
	test_codeword_api_rejects();
	test_codeword_api_bounds();
	test_codeword_api_matches_socket_send();
	test_codeword_api_prints_codeword();

	return MT_REPORT();
}
