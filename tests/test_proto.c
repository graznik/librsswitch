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
 * Unit tests for rsswitchd_proto.c/.h: no socket, no GPIO, no syslog.
 * x86_64.h is linked in only to prove that (test_err_names below), not
 * because the parser itself calls into the board or the library.
 */
#include <inttypes.h>
#include <string.h>

#include "minitest.h"
#include "../examples/network/rsswitchd_proto.h"
#include "../boards/x86_64/x86_64.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

/*
 * Feeds bytes[0..n) through pc in one proto_next() loop, collecting
 * up to max results in out[]. Returns the number of results.
 */
static size_t feed(struct proto_conn *pc, const char *bytes, size_t n,
		    struct proto_cmd *out, size_t max)
{
	const char *p = bytes;
	size_t left = n;
	size_t count = 0;

	while (count < max) {
		if (!proto_next(pc, &p, &left, &out[count]))
			break;
		count++;
	}

	return count;
}

/* Same as feed(), but calls proto_next() once per byte, so a command
 * split across many single-byte reads is exercised.
 */
static size_t feed_bytes(struct proto_conn *pc, const char *bytes, size_t n,
			  struct proto_cmd *out, size_t max)
{
	size_t count = 0;
	size_t i;

	for (i = 0; i < n; i++) {
		const char *p = &bytes[i];
		size_t left = 1;
		struct proto_cmd discard;
		struct proto_cmd *dst = count < max ? &out[count] : &discard;

		if (proto_next(pc, &p, &left, dst) && count < max)
			count++;
	}

	return count;
}

static void check_switch(const struct proto_cmd *cmd, uint32_t dev,
			  uint32_t group, uint32_t socket, uint32_t data)
{
	CHECK_EQ(cmd->kind, PROTO_SWITCH);
	CHECK_EQ(cmd->dev, dev);
	CHECK_EQ(cmd->group, group);
	CHECK_EQ(cmd->socket, socket);
	CHECK_EQ(cmd->data, data);
}

static void check_error(const struct proto_cmd *cmd, enum proto_err err)
{
	CHECK_EQ(cmd->kind, PROTO_ERROR);
	CHECK_EQ(cmd->err, err);
}

static void check_empty(const struct proto_cmd *cmd)
{
	CHECK_EQ(cmd->kind, PROTO_EMPTY);
}

static void test_worked_examples(void)
{
	struct proto_conn pc;
	struct proto_cmd cmd[4];
	size_t n;

	proto_init(&pc);
	{
		static const char in[] = "0 3 0 0\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_switch(&cmd[0], 0, 3, 0, 0);

	proto_init(&pc);
	{
		static const char in[] = "0 3 0 0\r\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_switch(&cmd[0], 0, 3, 0, 0);

	proto_init(&pc);
	{
		static const char in[] = "0 3 0 0\n0 3 0 1\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)2);
	check_switch(&cmd[0], 0, 3, 0, 0);
	check_switch(&cmd[1], 0, 3, 0, 1);

	proto_init(&pc);
	{
		static const char in[] = "1 17 23 1\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_switch(&cmd[0], 1, 17, 23, 1);

	proto_init(&pc);
	{
		static const char in[] = "1 017 23 1\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_SYNTAX);
	CHECK_EQ(strcmp(cmd[0].text, "malformed command"), 0);

	proto_init(&pc);
	{
		char big[200];
		static const char tail[] = "\n0 3 0 0\n";

		memset(big, 'x', sizeof(big));
		n = feed(&pc, big, sizeof(big), cmd, ARRAY_SIZE(cmd));
		CHECK_EQ(n, (size_t)1);
		check_error(&cmd[0], PROTO_ERR_TOOLONG);
		CHECK_EQ(strcmp(cmd[0].text, "line longer than 128 bytes"), 0);

		n = feed(&pc, tail, sizeof(tail) - 1, cmd, ARRAY_SIZE(cmd));
		CHECK_EQ(n, (size_t)1);
		check_switch(&cmd[0], 0, 3, 0, 0);
	}

	proto_init(&pc);
	n = feed(&pc, "\n", 1, cmd, ARRAY_SIZE(cmd));
	CHECK_EQ(n, (size_t)1);
	check_empty(&cmd[0]);

	proto_init(&pc);
	n = feed(&pc, "\r\n", 2, cmd, ARRAY_SIZE(cmd));
	CHECK_EQ(n, (size_t)1);
	check_empty(&cmd[0]);
}

static void test_fragmented(void)
{
	static const struct {
		const char *bytes;
		size_t len;
	} whole[] = {
		{ "0300\n", sizeof("0300\n") - 1 },
		{ "0300\r\n", sizeof("0300\r\n") - 1 },
		{ "03000301\n", sizeof("03000301\n") - 1 },
		{ "0 3 0 0\r\n", sizeof("0 3 0 0\r\n") - 1 },
		{ "1 17 23 1\n", sizeof("1 17 23 1\n") - 1 },
		{ "0 3 0 0\n", sizeof("0 3 0 0\n") - 1 },
		{ "1 017 23 1\n", sizeof("1 017 23 1\n") - 1 },
		{ "\n", 1 },
		{ "\r\n", 2 },
		{ "12345\n", sizeof("12345\n") - 1 },
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(whole); i++) {
		struct proto_conn pc_whole, pc_bytes;
		struct proto_cmd whole_cmd[4], bytes_cmd[4];
		size_t n_whole, n_bytes, j;

		proto_init(&pc_whole);
		n_whole = feed(&pc_whole, whole[i].bytes, whole[i].len,
			       whole_cmd, ARRAY_SIZE(whole_cmd));

		proto_init(&pc_bytes);
		n_bytes = feed_bytes(&pc_bytes, whole[i].bytes, whole[i].len,
				     bytes_cmd, ARRAY_SIZE(bytes_cmd));

		CHECK_EQ(n_whole, n_bytes);
		for (j = 0; j < n_whole && j < n_bytes; j++) {
			CHECK_EQ(whole_cmd[j].kind, bytes_cmd[j].kind);
			if (whole_cmd[j].kind == PROTO_SWITCH) {
				CHECK_EQ(whole_cmd[j].dev, bytes_cmd[j].dev);
				CHECK_EQ(whole_cmd[j].group,
					 bytes_cmd[j].group);
				CHECK_EQ(whole_cmd[j].socket,
					 bytes_cmd[j].socket);
				CHECK_EQ(whole_cmd[j].data, bytes_cmd[j].data);
			} else if (whole_cmd[j].kind == PROTO_ERROR) {
				CHECK_EQ(whole_cmd[j].err, bytes_cmd[j].err);
				CHECK_EQ(strcmp(whole_cmd[j].text,
						bytes_cmd[j].text), 0);
			}
		}
	}

	/* A v2 line directly followed by an empty line, a malformed line
	 * and a 4-digit line, all in one feed.
	 */
	{
		struct proto_conn pc;
		struct proto_cmd cmd[4];
		static const char in[] = "1 17 23 1\n\nx\n0300\n";
		size_t n;

		proto_init(&pc);
		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
		CHECK_EQ(n, (size_t)4);
		check_switch(&cmd[0], 1, 17, 23, 1);
		check_empty(&cmd[1]);
		check_error(&cmd[2], PROTO_ERR_SYNTAX);
		check_error(&cmd[3], PROTO_ERR_SYNTAX);
	}
}

/*
 * A line of four digits is an ordinary one-field line: it waits for
 * its line end like any other partial line, then it is a syntax error
 * and sends nothing. The parser stays usable afterwards.
 */
static void test_four_digits_are_a_syntax_error(void)
{
	static const char *const lines[] = { "0300\n", "0300\r\n", "12345\n" };
	struct proto_conn pc;
	struct proto_cmd cmd[4];
	size_t n;
	size_t i;

	/* 0300 without a line end is not executed: it waits. */
	proto_init(&pc);
	n = feed(&pc, "030", 3, cmd, ARRAY_SIZE(cmd));
	CHECK_EQ(n, (size_t)0);
	n = feed(&pc, "0", 1, cmd, ARRAY_SIZE(cmd));
	CHECK_EQ(n, (size_t)0);
	n = feed(&pc, "\n", 1, cmd, ARRAY_SIZE(cmd));
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_SYNTAX);
	CHECK_EQ(strcmp(cmd[0].text, "malformed command"), 0);

	n = feed(&pc, "0 3 0 0\n", 8, cmd, ARRAY_SIZE(cmd));
	CHECK_EQ(n, (size_t)1);
	check_switch(&cmd[0], 0, 3, 0, 0);

	/* The same with a CRLF, and with a fifth digit. */
	for (i = 0; i < ARRAY_SIZE(lines); i++) {
		proto_init(&pc);
		n = feed(&pc, lines[i], strlen(lines[i]), cmd,
			 ARRAY_SIZE(cmd));
		CHECK_EQ(n, (size_t)1);
		check_error(&cmd[0], PROTO_ERR_SYNTAX);
		CHECK_EQ(strcmp(cmd[0].text, "malformed command"), 0);

		n = feed(&pc, "0 3 0 0\n", 8, cmd, ARRAY_SIZE(cmd));
		CHECK_EQ(n, (size_t)1);
		check_switch(&cmd[0], 0, 3, 0, 0);
	}

	/* Two 4-digit lines back to back: two errors, no command. */
	proto_init(&pc);
	n = feed(&pc, "0300\n0301\n", 10, cmd, ARRAY_SIZE(cmd));
	CHECK_EQ(n, (size_t)2);
	check_error(&cmd[0], PROTO_ERR_SYNTAX);
	check_error(&cmd[1], PROTO_ERR_SYNTAX);

	/* Four digits and a lone \r wait for a \n; the \r is part of
	 * the line, so x\n after it is one more syntax error.
	 */
	proto_init(&pc);
	n = feed(&pc, "0300\r", 5, cmd, ARRAY_SIZE(cmd));
	CHECK_EQ(n, (size_t)0);
	n = feed(&pc, "x\n", 2, cmd, ARRAY_SIZE(cmd));
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_SYNTAX);
}

static void test_v2_syntax(void)
{
	static const struct {
		const char *bytes;
		size_t len;
	} bad[] = {
		{ " 1 17 23 1\n", sizeof(" 1 17 23 1\n") - 1 },
		{ "1 17 23 1 \n", sizeof("1 17 23 1 \n") - 1 },
		{ "1  17 23 1\n", sizeof("1  17 23 1\n") - 1 },
		{ "1\t17 23 1\n", sizeof("1\t17 23 1\n") - 1 },
		{ "1 17 23\n", sizeof("1 17 23\n") - 1 },
		{ "1 17 23 1 0\n", sizeof("1 17 23 1 0\n") - 1 },
		{ " \n", sizeof(" \n") - 1 },
		{ "1 -1 23 1\n", sizeof("1 -1 23 1\n") - 1 },
		{ "1 +1 23 1\n", sizeof("1 +1 23 1\n") - 1 },
		{ "1 1a 23 1\n", sizeof("1 1a 23 1\n") - 1 },
		{ "1 0x11 23 1\n", sizeof("1 0x11 23 1\n") - 1 },
		{ "1 01 23 1\n", sizeof("1 01 23 1\n") - 1 },
		{ "1 00 23 1\n", sizeof("1 00 23 1\n") - 1 },
		{ "1 \xEF\xBC\x90 23 1\n", sizeof("1 \xEF\xBC\x90 23 1\n") - 1 },
		{ "1 \x00 23 1\n", sizeof("1 \x00 23 1\n") - 1 },
		{ "1 aBc 23 1\n", sizeof("1 aBc 23 1\n") - 1 },
	};
	struct proto_conn pc;
	struct proto_cmd cmd[4];
	size_t i;

	for (i = 0; i < ARRAY_SIZE(bad); i++) {
		size_t n;

		proto_init(&pc);
		n = feed(&pc, bad[i].bytes, bad[i].len, cmd, ARRAY_SIZE(cmd));
		CHECK_EQ(n, (size_t)1);
		check_error(&cmd[0], PROTO_ERR_SYNTAX);
	}

	{
		static const char ok[] = "1 0 0 0\n";
		size_t n;

		proto_init(&pc);
		n = feed(&pc, ok, sizeof(ok) - 1, cmd, ARRAY_SIZE(cmd));
		CHECK_EQ(n, (size_t)1);
		check_switch(&cmd[0], 1, 0, 0, 0);
	}
}

static void test_overflow(void)
{
	struct proto_conn pc;
	struct proto_cmd cmd[4];
	size_t n;

	proto_init(&pc);
	{
		static const char in[] = "1 4294967296 0 1\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_RANGE);
	CHECK_EQ(strcmp(cmd[0].text, "group too large"), 0);

	proto_init(&pc);
	{
		static const char in[] = "1 99999999999999999999 0 1\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_RANGE);
	CHECK_EQ(strcmp(cmd[0].text, "group too large"), 0);

	/* 4294967295 fits uint32_t (it is UINT32_MAX), so conversion
	 * succeeds -- but it is nowhere near a valid Pollin group, so
	 * this is now a range error naming the parsed number, not a
	 * conversion failure or a wrapped/truncated value.
	 */
	proto_init(&pc);
	{
		static const char in[] = "1 4294967295 0 1\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_RANGE);
	CHECK_EQ(strcmp(cmd[0].text, "group 4294967295 not in 0-31"), 0);
}

/*
 * Every field is checked against socket_addr_limits(),
 * in dev/group/socket/data order, and the range check never opens a
 * board session.
 */
static void test_ranges(void)
{
	struct proto_conn pc;
	struct proto_cmd cmd[4];
	size_t n;

	x86_64_recorder_reset();

	proto_init(&pc);
	{
		static const char in[] = "0 3 2 1\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_switch(&cmd[0], 0, 3, 2, 1);

	proto_init(&pc);
	{
		static const char in[] = "0 4 0 0\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_RANGE);
	CHECK_EQ(strcmp(cmd[0].text, "group 4 not in 0-3"), 0);

	proto_init(&pc);
	{
		static const char in[] = "0 0 3 0\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_RANGE);
	CHECK_EQ(strcmp(cmd[0].text, "socket 3 not in 0-2"), 0);

	proto_init(&pc);
	{
		static const char in[] = "0 0 0 2\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_RANGE);
	CHECK_EQ(strcmp(cmd[0].text, "data 2 not in 0-1"), 0);

	proto_init(&pc);
	{
		static const char in[] = "2 0 0 0\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_RANGE);
	CHECK_EQ(strcmp(cmd[0].text, "dev 2 unknown"), 0);

	proto_init(&pc);
	{
		static const char in[] = "1 31 31 1\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_switch(&cmd[0], 1, 31, 31, 1);

	proto_init(&pc);
	{
		static const char in[] = "1 32 0 0\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_RANGE);
	CHECK_EQ(strcmp(cmd[0].text, "group 32 not in 0-31"), 0);

	proto_init(&pc);
	{
		static const char in[] = "1 0 32 0\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_RANGE);
	CHECK_EQ(strcmp(cmd[0].text, "socket 32 not in 0-31"), 0);

	proto_init(&pc);
	{
		static const char in[] = "1 10 10 1\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_switch(&cmd[0], 1, 10, 10, 1);

	/* dev 9 is unknown. */
	proto_init(&pc);
	{
		static const char in[] = "9 9 9 9\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_RANGE);
	CHECK_EQ(strcmp(cmd[0].text, "dev 9 unknown"), 0);

	/* POLLIN_2605, group 9 and socket 9 both valid, data 9 not in
	 * 0-1.
	 */
	proto_init(&pc);
	{
		static const char in[] = "1 9 9 9\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_RANGE);
	CHECK_EQ(strcmp(cmd[0].text, "data 9 not in 0-1"), 0);

	/* dev is checked before group, socket or data, even though
	 * all three are also out of range.
	 */
	proto_init(&pc);
	{
		static const char in[] = "2 40 0 9\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_RANGE);
	CHECK_EQ(strcmp(cmd[0].text, "dev 2 unknown"), 0);

	/* Syntax beats range: a leading zero in "017" is a syntax error,
	 * even though the line also names a dev-2 that would otherwise
	 * be a range error.
	 */
	proto_init(&pc);
	{
		static const char in[] = "2 017 0 1\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_SYNTAX);

	proto_init(&pc);
	{
		static const char in[] = "1 4294967295 0 1\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_RANGE);
	CHECK_EQ(strcmp(cmd[0].text, "group 4294967295 not in 0-31"), 0);

	CHECK_EQ(x86_64_board_init_count(), (size_t)0);
}

/*
 * The parser's range check follows socket_addr_limits() exactly, not
 * a copy of it: for both devices, every (group, socket, data) inside
 * the reported limits parses to a switch, and each field one past its
 * reported limit is a range error -- with no board session ever
 * opened.
 */
static void test_ranges_follow_library(void)
{
	static const uint32_t devs[] = { REV_008345, POLLIN_2605 };
	size_t d;

	x86_64_recorder_reset();

	for (d = 0; d < ARRAY_SIZE(devs); d++) {
		uint32_t ngroups, nsockets, ndata;
		uint32_t g, s, v;
		struct proto_conn pc;
		struct proto_cmd cmd;

		CHECK_EQ(socket_addr_limits(devs[d], &ngroups, &nsockets,
					     &ndata), 0);

		proto_init(&pc);
		for (g = 0; g < ngroups; g++) {
			for (s = 0; s < nsockets; s++) {
				for (v = 0; v < ndata; v++) {
					char line[32];
					const char *p = line;
					size_t left;
					int len;

					len = snprintf(line, sizeof(line),
						       "%" PRIu32 " %" PRIu32
						       " %" PRIu32 " %" PRIu32
						       "\n",
						       devs[d], g, s, v);
					left = (size_t)len;
					CHECK_EQ(proto_next(&pc, &p, &left,
							     &cmd), 1);
					check_switch(&cmd, devs[d], g, s, v);
				}
			}
		}

		{
			char line[32];
			const char *p;
			size_t left;
			int len;

			len = snprintf(line, sizeof(line),
				       "%" PRIu32 " %" PRIu32 " 0 0\n",
				       devs[d], ngroups);
			p = line;
			left = (size_t)len;
			CHECK_EQ(proto_next(&pc, &p, &left, &cmd), 1);
			check_error(&cmd, PROTO_ERR_RANGE);

			len = snprintf(line, sizeof(line),
				       "%" PRIu32 " 0 %" PRIu32 " 0\n",
				       devs[d], nsockets);
			p = line;
			left = (size_t)len;
			CHECK_EQ(proto_next(&pc, &p, &left, &cmd), 1);
			check_error(&cmd, PROTO_ERR_RANGE);

			len = snprintf(line, sizeof(line),
				       "%" PRIu32 " 0 0 %" PRIu32 "\n",
				       devs[d], ndata);
			p = line;
			left = (size_t)len;
			CHECK_EQ(proto_next(&pc, &p, &left, &cmd), 1);
			check_error(&cmd, PROTO_ERR_RANGE);
		}
	}

	CHECK_EQ(x86_64_board_init_count(), (size_t)0);
}

static void check_raw(const struct proto_cmd *cmd, const char *tristate,
		       uint32_t pulse_len)
{
	CHECK_EQ(cmd->kind, PROTO_RAW);
	CHECK_EQ(strcmp(cmd->tristate, tristate), 0);
	CHECK_EQ(cmd->pulse_len, pulse_len);
}

/*
 * The raw grammar: well-formed lines produce PROTO_RAW with
 * the tristate copied exactly and pulse_len parsed; every malformed
 * or out-of-range shape is rejected with the right class, syntax
 * always winning over range for an over-length tristate that also
 * has an invalid character. Fed whole and one byte at a time, with
 * identical results either way.
 */
static void test_raw(void)
{
	static const struct {
		const char *bytes;
		size_t len;
		const char *tristate;
		uint32_t pulse_len;
	} ok[] = {
		{ "raw 0FFF0000F00F 350\n",
		  sizeof("raw 0FFF0000F00F 350\n") - 1, "0FFF0000F00F", 350 },
		{ "raw 0 50\n", sizeof("raw 0 50\n") - 1, "0", 50 },
		{ "raw 1F 2000\n", sizeof("raw 1F 2000\n") - 1, "1F", 2000 },
	};
	static const struct {
		const char *bytes;
		size_t len;
	} syntax_bad[] = {
		{ "raw 0FFF0000F00F\n", sizeof("raw 0FFF0000F00F\n") - 1 },
		{ "raw 350\n", sizeof("raw 350\n") - 1 },
		{ "raw\n", sizeof("raw\n") - 1 },
		{ "raw 0 350 1\n", sizeof("raw 0 350 1\n") - 1 },
		{ "RAW 0 350\n", sizeof("RAW 0 350\n") - 1 },
		{ "raw 0f 350\n", sizeof("raw 0f 350\n") - 1 },
		{ "raw 02 350\n", sizeof("raw 02 350\n") - 1 },
		{ "raw  350\n", sizeof("raw  350\n") - 1 },
		{ "raw 0 0350\n", sizeof("raw 0 0350\n") - 1 },
		{ "raw 0 -1\n", sizeof("raw 0 -1\n") - 1 },
		{ "raw 0 350 \n", sizeof("raw 0 350 \n") - 1 },
		/* 13 tristate bytes, one a lowercase 'f': syntax wins
		 * over the length-13 range violation.
		 */
		{ "raw 0FFF0000F00f0 350\n",
		  sizeof("raw 0FFF0000F00f0 350\n") - 1 },
	};
	struct proto_conn pc;
	struct proto_cmd cmd[4];
	size_t i, n;

	for (i = 0; i < ARRAY_SIZE(ok); i++) {
		proto_init(&pc);
		n = feed(&pc, ok[i].bytes, ok[i].len, cmd, ARRAY_SIZE(cmd));
		CHECK_EQ(n, (size_t)1);
		check_raw(&cmd[0], ok[i].tristate, ok[i].pulse_len);

		proto_init(&pc);
		n = feed_bytes(&pc, ok[i].bytes, ok[i].len, cmd,
			       ARRAY_SIZE(cmd));
		CHECK_EQ(n, (size_t)1);
		check_raw(&cmd[0], ok[i].tristate, ok[i].pulse_len);
	}

	for (i = 0; i < ARRAY_SIZE(syntax_bad); i++) {
		proto_init(&pc);
		n = feed(&pc, syntax_bad[i].bytes, syntax_bad[i].len, cmd,
			 ARRAY_SIZE(cmd));
		CHECK_EQ(n, (size_t)1);
		check_error(&cmd[0], PROTO_ERR_SYNTAX);

		proto_init(&pc);
		n = feed_bytes(&pc, syntax_bad[i].bytes, syntax_bad[i].len,
			       cmd, ARRAY_SIZE(cmd));
		CHECK_EQ(n, (size_t)1);
		check_error(&cmd[0], PROTO_ERR_SYNTAX);
	}

	/* 13 valid tristate bytes (no invalid character): a range
	 * violation, not syntax.
	 */
	proto_init(&pc);
	{
		static const char in[] = "raw 0FFF0000F00F0 350\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_RANGE);
	CHECK_EQ(strcmp(cmd[0].text, "tristate longer than 12"), 0);

	proto_init(&pc);
	{
		static const char in[] = "raw 0 49\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_RANGE);
	CHECK_EQ(strcmp(cmd[0].text, "pulse_len 49 not in 50-2000"), 0);

	proto_init(&pc);
	{
		static const char in[] = "raw 0 2001\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_RANGE);
	CHECK_EQ(strcmp(cmd[0].text, "pulse_len 2001 not in 50-2000"), 0);

	proto_init(&pc);
	{
		static const char in[] = "raw 0 99999999999\n";

		n = feed(&pc, in, sizeof(in) - 1, cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_RANGE);
	CHECK_EQ(strcmp(cmd[0].text, "pulse_len too large"), 0);

	/* A 200-byte raw line with no newline: the generic line-length
	 * bound fires, same as any other overlong line.
	 */
	proto_init(&pc);
	{
		char big[200];

		memset(big, '0', sizeof(big));
		memcpy(big, "raw ", 4);
		n = feed(&pc, big, sizeof(big), cmd, ARRAY_SIZE(cmd));
	}
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_TOOLONG);
}

static void test_line_limit(void)
{
	struct proto_conn pc;
	struct proto_cmd cmd[64];
	char buf[260];
	size_t n;

	/* 128 x plus \n: a full-length line, judged by the grammar. */
	proto_init(&pc);
	memset(buf, 'x', 128);
	buf[128] = '\n';
	n = feed(&pc, buf, 129, cmd, ARRAY_SIZE(cmd));
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_SYNTAX);

	/* 129 x, no newline: toolong fires at once. */
	proto_init(&pc);
	memset(buf, 'x', 129);
	n = feed(&pc, buf, 129, cmd, ARRAY_SIZE(cmd));
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_TOOLONG);

	/* 127 x plus \r\n: the \r is counted but still fits (a line,
	 * not toolong).
	 */
	proto_init(&pc);
	memset(buf, 'x', 127);
	buf[127] = '\r';
	buf[128] = '\n';
	n = feed(&pc, buf, 129, cmd, ARRAY_SIZE(cmd));
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_SYNTAX);

	/* 128 x plus \r\n: the \r pushes it over the limit. */
	proto_init(&pc);
	memset(buf, 'x', 128);
	buf[128] = '\r';
	buf[129] = '\n';
	n = feed(&pc, buf, 130, cmd, ARRAY_SIZE(cmd));
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_TOOLONG);

	/* 200 bytes of the digit 1, no newline: one toolong error, not
	 * a run of commands; the next line is then parsed normally.
	 */
	proto_init(&pc);
	memset(buf, '1', 200);
	n = feed(&pc, buf, 200, cmd, ARRAY_SIZE(cmd));
	CHECK_EQ(n, (size_t)1);
	check_error(&cmd[0], PROTO_ERR_TOOLONG);

	n = feed(&pc, "\n1 1 1 1\n", 9, cmd, ARRAY_SIZE(cmd));
	CHECK_EQ(n, (size_t)1);
	check_switch(&cmd[0], 1, 1, 1, 1);
}

static void test_zero_input(void)
{
	struct proto_conn pc;
	struct proto_cmd cmd;
	const char *p = "";
	size_t len = 0;
	int rc;

	proto_init(&pc);
	rc = proto_next(&pc, &p, &len, &cmd);
	CHECK_EQ(rc, 0);
	CHECK_EQ(pc.len, (size_t)0);
	CHECK_EQ(pc.discarding, 0);
}

static void test_independent_state(void)
{
	struct proto_conn a, b, solo;
	struct proto_cmd cmd_a[4], cmd_b[4];
	struct proto_cmd solo_a[4], solo_b[4];
	static const char msg_a[] = "0 3 0 0\n";
	static const char msg_b[] = "1 17 23 1\n";
	size_t na = 0, nb = 0;
	size_t max_len, i, nsolo;

	proto_init(&a);
	proto_init(&b);

	max_len = sizeof(msg_a) - 1;
	if (sizeof(msg_b) - 1 > max_len)
		max_len = sizeof(msg_b) - 1;

	for (i = 0; i < max_len; i++) {
		if (i < sizeof(msg_a) - 1) {
			const char *p = &msg_a[i];
			size_t len = 1;

			if (proto_next(&a, &p, &len, &cmd_a[na]))
				na++;
		}
		if (i < sizeof(msg_b) - 1) {
			const char *p = &msg_b[i];
			size_t len = 1;

			if (proto_next(&b, &p, &len, &cmd_b[nb]))
				nb++;
		}
	}

	proto_init(&solo);
	nsolo = feed(&solo, msg_a, sizeof(msg_a) - 1, solo_a,
		     ARRAY_SIZE(solo_a));
	CHECK_EQ(na, nsolo);
	for (i = 0; i < na && i < nsolo; i++)
		CHECK_EQ(cmd_a[i].kind, solo_a[i].kind);

	proto_init(&solo);
	nsolo = feed(&solo, msg_b, sizeof(msg_b) - 1, solo_b,
		     ARRAY_SIZE(solo_b));
	CHECK_EQ(nb, nsolo);
	for (i = 0; i < nb && i < nsolo; i++)
		CHECK_EQ(cmd_b[i].kind, solo_b[i].kind);
}

static int text_is_clean(const char *text)
{
	size_t i;

	for (i = 0; text[i] != '\0'; i++) {
		unsigned char c = (unsigned char)text[i];

		if (c < 0x20 || c > 0x7e)
			return 0;
	}
	return 1;
}

static void test_texts_are_clean(void)
{
	static const struct {
		const char *bytes;
		size_t len;
	} inputs[] = {
		{ "\x1b[31m\n", sizeof("\x1b[31m\n") - 1 },
		{ "zzzz\n", sizeof("zzzz\n") - 1 },
		{ "1 \xFF 23 1\n", sizeof("1 \xFF 23 1\n") - 1 },
	};
	struct proto_conn pc;
	struct proto_cmd cmd[4];
	size_t i;

	for (i = 0; i < ARRAY_SIZE(inputs); i++) {
		size_t j, n;

		proto_init(&pc);
		n = feed(&pc, inputs[i].bytes, inputs[i].len, cmd,
			 ARRAY_SIZE(cmd));
		CHECK(n >= 1);
		for (j = 0; j < n; j++) {
			if (cmd[j].kind == PROTO_ERROR) {
				CHECK(text_is_clean(cmd[j].text));
				CHECK(strstr(cmd[j].text, "\x1b") == NULL);
			}
		}
	}
}

static void test_err_names(void)
{
	CHECK_EQ(strcmp(proto_err_name(PROTO_ERR_SYNTAX), "syntax"), 0);
	CHECK_EQ(strcmp(proto_err_name(PROTO_ERR_RANGE), "range"), 0);
	CHECK_EQ(strcmp(proto_err_name(PROTO_ERR_TOOLONG), "toolong"), 0);
	CHECK_EQ(strcmp(proto_err_name(PROTO_ERR_SEND), "send"), 0);

	/* The parser never opens a board. */
	CHECK_EQ(x86_64_board_init_count(), (size_t)0);
}

int main(void)
{
	test_worked_examples();
	test_fragmented();
	test_four_digits_are_a_syntax_error();
	test_v2_syntax();
	test_overflow();
	test_ranges();
	test_ranges_follow_library();
	test_raw();
	test_line_limit();
	test_zero_input();
	test_independent_state();
	test_texts_are_clean();
	test_err_names();

	return MT_REPORT();
}
