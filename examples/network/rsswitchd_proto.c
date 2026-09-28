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
 * Implements the state machine and grammar declared in
 * rsswitchd_proto.h. See that file for the full contract.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rsswitchd_proto.h"

/* '0'..'9' by explicit range comparison -- never the locale-dependent
 * isdigit(), which could accept other bytes on some locales.
 */
static int is_ascii_digit(char c)
{
	return c >= '0' && c <= '9';
}

/*
 * Splits line (len bytes, not NUL-terminated, may contain any byte)
 * into exactly 4 fields separated by single spaces, storing each
 * field's start pointer and length in field[]/flen[]. A leading or
 * trailing space, two spaces in a row (which yields a zero-length
 * field), or any field count other than exactly 4 all fail this
 * check -- the caller has no need to tell those shapes apart, they
 * are all "malformed command". Returns 1 on exactly 4 fields, 0
 * otherwise.
 */
static int split_fields(const char *line, size_t len,
			 const char *field[4], size_t flen[4])
{
	size_t start = 0;
	size_t nfields = 0;
	size_t i;

	for (i = 0; i <= len; i++) {
		if (i == len || line[i] == ' ') {
			if (nfields >= 4)
				return 0;
			field[nfields] = line + start;
			flen[nfields] = i - start;
			nfields++;
			start = i + 1;
		}
	}

	return nfields == 4;
}

/*
 * A field is well-formed for v2 (D-07): only ASCII digits, and,
 * unless it is the single character "0", no leading zero. Digit
 * count and numeric range are not checked here -- those are the
 * RANGE class, handled by field_to_u32() below.
 */
static int field_ok(const char *s, size_t len)
{
	size_t i;

	if (len == 0)
		return 0;

	for (i = 0; i < len; i++) {
		if (!is_ascii_digit(s[i]))
			return 0;
	}

	if (len > 1 && s[0] == '0')
		return 0;

	return 1;
}

/*
 * Converts a field already accepted by field_ok() to a uint32_t.
 * Fails (returns -1) on more than 10 digits, strtoul()'s ERANGE, or a
 * value above UINT32_MAX -- all three are RANGE failures (D-11): the
 * field is well-formed digits, just too large a number.
 */
static int field_to_u32(const char *s, size_t len, uint32_t *out)
{
	char tmp[12];
	unsigned long val;
	char *end;

	if (len > 10)
		return -1;

	memcpy(tmp, s, len);
	tmp[len] = '\0';

	errno = 0;
	val = strtoul(tmp, &end, 10);
	if (errno == ERANGE || val > UINT32_MAX || *end != '\0')
		return -1;

	*out = (uint32_t)val;
	return 0;
}

/*
 * Parses one already-delimited line (len > 0, no trailing \r or \n)
 * into *cmd. "Syntax before range, first failing field wins": every
 * field's syntax is checked first; only once all four pass are they
 * converted, in field order (dev, group, socket, data), and the
 * first one whose value does not fit wins the RANGE error. Plan
 * 02-03 adds the per-device bounds check once conversion succeeds.
 */
static void parse_line(const char *line, size_t len, struct proto_cmd *cmd)
{
	static const char *const names[4] = { "dev", "group", "socket",
					       "data" };
	const char *field[4];
	size_t flen[4];
	uint32_t val[4];
	size_t i;

	if (!split_fields(line, len, field, flen)) {
		cmd->kind = PROTO_ERROR;
		cmd->err = PROTO_ERR_SYNTAX;
		snprintf(cmd->text, sizeof(cmd->text), "malformed command");
		return;
	}

	for (i = 0; i < 4; i++) {
		if (!field_ok(field[i], flen[i])) {
			cmd->kind = PROTO_ERROR;
			cmd->err = PROTO_ERR_SYNTAX;
			snprintf(cmd->text, sizeof(cmd->text),
				 "malformed command");
			return;
		}
	}

	for (i = 0; i < 4; i++) {
		if (field_to_u32(field[i], flen[i], &val[i]) < 0) {
			cmd->kind = PROTO_ERROR;
			cmd->err = PROTO_ERR_RANGE;
			snprintf(cmd->text, sizeof(cmd->text),
				 "%s too large", names[i]);
			return;
		}
	}

	cmd->kind = PROTO_SWITCH;
	cmd->legacy = 0;
	cmd->dev = val[0];
	cmd->group = val[1];
	cmd->socket = val[2];
	cmd->data = val[3];
}

void proto_init(struct proto_conn *pc)
{
	pc->len = 0;
	pc->discarding = 0;
	pc->legacy_tail = 0;
}

int proto_next(struct proto_conn *pc, const char **in, size_t *inlen,
	       struct proto_cmd *cmd)
{
	while (*inlen > 0) {
		unsigned char c = (unsigned char)**in;

		if (pc->discarding) {
			(*in)++;
			(*inlen)--;
			if (c == '\n')
				pc->discarding = 0;
			continue;
		}

		if (pc->legacy_tail) {
			if (pc->len == 0) {
				if (c == '\n') {
					(*in)++;
					(*inlen)--;
					pc->legacy_tail = 0;
					continue;
				}
				if (c == '\r') {
					(*in)++;
					(*inlen)--;
					pc->buf[0] = '\r';
					pc->len = 1;
					continue;
				}
				/* Neither: starts the next command,
				 * unconsumed -- fall through below.
				 */
				pc->legacy_tail = 0;
			} else {
				/* pc->len == 1, buf[0] == '\r': a lone
				 * \r was pending the byte after it.
				 */
				if (c == '\n') {
					(*in)++;
					(*inlen)--;
					pc->legacy_tail = 0;
					pc->len = 0;
					continue;
				}
				/* Not a CRLF: the buffered \r starts
				 * the next command, together with this
				 * byte -- fall through below, which
				 * appends it to the already-buffered
				 * \r.
				 */
				pc->legacy_tail = 0;
			}
		}

		if (c == '\n') {
			size_t linelen = pc->len;

			(*in)++;
			(*inlen)--;

			if (linelen > 0 && pc->buf[linelen - 1] == '\r')
				linelen--;

			pc->len = 0;

			if (linelen == 0) {
				cmd->kind = PROTO_EMPTY;
				return 1;
			}

			parse_line(pc->buf, linelen, cmd);
			return 1;
		}

		if (pc->len >= PROTO_MAX_LINE) {
			(*in)++;
			(*inlen)--;
			pc->len = 0;
			pc->discarding = 1;
			cmd->kind = PROTO_ERROR;
			cmd->err = PROTO_ERR_TOOLONG;
			snprintf(cmd->text, sizeof(cmd->text),
				 "line longer than %u bytes", PROTO_MAX_LINE);
			return 1;
		}

		pc->buf[pc->len++] = (char)c;
		(*in)++;
		(*inlen)--;

		if (pc->len == 4 &&
		    is_ascii_digit(pc->buf[0]) &&
		    is_ascii_digit(pc->buf[1]) &&
		    is_ascii_digit(pc->buf[2]) &&
		    is_ascii_digit(pc->buf[3])) {
			cmd->kind = PROTO_SWITCH;
			cmd->legacy = 1;
			cmd->dev = (uint32_t)(pc->buf[0] - '0');
			cmd->group = (uint32_t)(pc->buf[1] - '0');
			cmd->socket = (uint32_t)(pc->buf[2] - '0');
			cmd->data = (uint32_t)(pc->buf[3] - '0');
			pc->len = 0;
			pc->legacy_tail = 1;
			return 1;
		}
	}

	return 0;
}

const char *proto_err_name(enum proto_err err)
{
	switch (err) {
	case PROTO_ERR_SYNTAX:
		return "syntax";
	case PROTO_ERR_RANGE:
		return "range";
	case PROTO_ERR_TOOLONG:
		return "toolong";
	case PROTO_ERR_SEND:
		return "send";
	default:
		return "unknown";
	}
}
