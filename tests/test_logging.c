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
 * Captures every syslog() call the library makes.
 * syslog is redefined to capture_syslog before ../librsswitch.c is
 * included directly, so every log call inside the library -- and the
 * DEBUG-only line in socket_send(), since this binary is built with
 * -DDEBUG (see tests/Makefile) -- is captured instead of going to the
 * real syslog. Included directly rather than linked, exactly like
 * tests/test_codeword_api.c, so this file can also reach the static
 * socket_ctrl() to test how it tells a truncated codeword from a
 * snprintf() failure.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#undef syslog
#define syslog capture_syslog

static void capture_syslog(int priority, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

#include "minitest.h"
#include "pulse_decode.h"
#include "../librsswitch.c"

#define CAPTURE_MAX_MESSAGES 64
#define CAPTURE_MSG_LEN 256

struct captured_message {
	int priority;
	char text[CAPTURE_MSG_LEN];
};

static struct captured_message captured[CAPTURE_MAX_MESSAGES];
static size_t captured_n;

/*
 * Formats and stores one captured syslog() call. Drops the message
 * once the table is full, instead of growing it or aborting, since
 * a full table already means something is wrong with the test that
 * should be visible via captured_count(), not a crash here.
 */
static void capture_syslog(int priority, const char *fmt, ...)
{
	va_list ap;

	if (captured_n >= CAPTURE_MAX_MESSAGES)
		return;

	va_start(ap, fmt);
	vsnprintf(captured[captured_n].text, sizeof(captured[captured_n].text),
		  fmt, ap);
	va_end(ap);

	captured[captured_n].priority = priority;
	captured_n++;
}

static void captured_reset(void)
{
	captured_n = 0;
}

static size_t captured_count(void)
{
	return captured_n;
}

/* Returns 1 if any captured message contains needle, 0 otherwise. */
static int captured_find(const char *needle)
{
	size_t i;

	for (i = 0; i < captured_n; i++) {
		if (strstr(captured[i].text, needle) != NULL)
			return 1;
	}

	return 0;
}

static void test_capture_is_not_vacuous(void)
{
	captured_reset();
	CHECK_EQ(captured_count(), (size_t)0);
	CHECK_EQ(captured_find("this message was never logged"), 0);

	x86_64_recorder_reset();
	CHECK_EQ(socket_send(REV_008345, 4294967295U, 0, 0), -1);
	CHECK(captured_count() > 0);
	CHECK_EQ(captured_find("this message was never logged"), 0);
}

static void test_uint32_values_logged_unsigned(void)
{
	captured_reset();
	CHECK_EQ(socket_send(REV_008345, 4294967295U, 0, 0), -1);
	CHECK(captured_find("[-] Unknown socket group 4294967295"));

	captured_reset();
	CHECK_EQ(socket_send(REV_008345, 0, 4294967295U, 0), -1);
	CHECK(captured_find("[-] Unknown socket 4294967295"));

	captured_reset();
	CHECK_EQ(socket_send(REV_008345, 0, 0, 4294967295U), -1);
	CHECK(captured_find("[-] Unknown switch parameter 4294967295"));

	captured_reset();
	CHECK_EQ(socket_send(POLLIN_2605, 4294967295U, 0, 0), -1);
	CHECK(captured_find("[-] Unknown socket group 4294967295"));

	captured_reset();
	CHECK_EQ(socket_send(POLLIN_2605, 0, 4294967295U, 0), -1);
	CHECK(captured_find("[-] Unknown socket 4294967295"));

	captured_reset();
	CHECK_EQ(socket_send(POLLIN_2605, 0, 0, 4294967295U), -1);
	CHECK(captured_find("[-] Unknown switch parameter 4294967295"));

	captured_reset();
	CHECK_EQ(socket_send(4294967295U, 0, 0, 0), -1);
	CHECK(captured_find("[-] Unknown socket type 4294967295"));

	captured_reset();
	CHECK_EQ(socket_send(4294967295U, 4294967295U, 4294967295U,
			      4294967295U), -1);
	CHECK(captured_find(
		"dev: 4294967295, group: 4294967295, socket: 4294967295, data: 4294967295"));

	captured_reset();
	CHECK_EQ(socket_send_codeword("0", 4294967295U), -1);
	CHECK(captured_find("4294967295"));
}

/*
 * socket_ctrl() must tell a too-long assembled codeword apart from a
 * genuine snprintf() failure, and it must reject both a wildly
 * oversized input (17 characters) and one just one byte over the
 * limit (13 characters) without transmitting anything.
 */
static void test_socket_ctrl_truncation(void)
{
	captured_reset();
	x86_64_recorder_reset();
	CHECK_EQ(socket_ctrl("1FFF1FF0", "00010", "FFFF", 350), -1);
	CHECK_EQ(x86_64_recorder_count(), (size_t)0);
	CHECK(captured_find("Codeword too long"));
	CHECK_EQ(captured_find("snprintf() failed"), 0);

	captured_reset();
	x86_64_recorder_reset();
	CHECK_EQ(socket_ctrl("1FFF", "1FF0", "00010", 350), -1);
	CHECK_EQ(x86_64_recorder_count(), (size_t)0);
	CHECK(captured_find("Codeword too long"));
}

/* An exactly-12-character assembled codeword is accepted and sent. */
static void test_socket_ctrl_max_length(void)
{
	x86_64_recorder_reset();
	CHECK_EQ(socket_ctrl("1FFF", "1FF0", "0001", 350), 0);
	CHECK_EQ(expect_transmission("1FFF1FF00001", 350), 0);
}

int main(void)
{
	test_capture_is_not_vacuous();
	test_uint32_values_logged_unsigned();
	test_socket_ctrl_truncation();
	test_socket_ctrl_max_length();

	return MT_REPORT();
}
