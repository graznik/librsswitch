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
#include <inttypes.h>
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
 * into fields separated by single spaces, storing each field's start
 * pointer and length in field[]/flen[] for up to max fields. Returns
 * the total number of fields found in the line, which may exceed
 * max -- a field at or beyond index max is counted but not stored, so
 * the caller can still tell "too many fields" apart from an exact
 * count without an unbounded array. A leading or trailing space, or
 * two spaces in a row, all still produce a (counted) zero-length
 * field.
 */
static size_t split_fields(const char *line, size_t len,
			    const char *field[], size_t flen[], size_t max)
{
	size_t start = 0;
	size_t nfields = 0;
	size_t i;

	for (i = 0; i <= len; i++) {
		if (i == len || line[i] == ' ') {
			if (nfields < max) {
				field[nfields] = line + start;
				flen[nfields] = i - start;
			}
			nfields++;
			start = i + 1;
		}
	}

	return nfields;
}

/*
 * A field is well-formed for v2: only ASCII digits, and,
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
 * value above UINT32_MAX -- all three are RANGE failures: the
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
 * Checks dev, group, socket and data against librsswitch.h's
 * socket_addr_limits(), in that field order, and fills
 * *cmd accordingly: PROTO_SWITCH when every field is in range, or
 * PROTO_ERROR/PROTO_ERR_RANGE naming the first out-of-range field
 * otherwise. The check always happens before socket_send() is ever
 * called -- which still checks again on its own.
 */
static void check_range(struct proto_cmd *cmd, uint32_t dev, uint32_t group,
			 uint32_t socket, uint32_t data)
{
	uint32_t ngroups, nsockets, ndata;

	if (socket_addr_limits(dev, &ngroups, &nsockets, &ndata) < 0) {
		cmd->kind = PROTO_ERROR;
		cmd->err = PROTO_ERR_RANGE;
		snprintf(cmd->text, sizeof(cmd->text), "dev %" PRIu32
			 " unknown", dev);
		return;
	}
	if (group >= ngroups) {
		cmd->kind = PROTO_ERROR;
		cmd->err = PROTO_ERR_RANGE;
		snprintf(cmd->text, sizeof(cmd->text), "group %" PRIu32
			 " not in 0-%" PRIu32, group, ngroups - 1);
		return;
	}
	if (socket >= nsockets) {
		cmd->kind = PROTO_ERROR;
		cmd->err = PROTO_ERR_RANGE;
		snprintf(cmd->text, sizeof(cmd->text), "socket %" PRIu32
			 " not in 0-%" PRIu32, socket, nsockets - 1);
		return;
	}
	if (data >= ndata) {
		cmd->kind = PROTO_ERROR;
		cmd->err = PROTO_ERR_RANGE;
		snprintf(cmd->text, sizeof(cmd->text), "data %" PRIu32
			 " not in 0-%" PRIu32, data, ndata - 1);
		return;
	}

	cmd->kind = PROTO_SWITCH;
	cmd->dev = dev;
	cmd->group = group;
	cmd->socket = socket;
	cmd->data = data;
}

/*
 * Parses the two already-delimited fields of "raw <tristate>
 * <pulse_len>" into *cmd. Syntax is checked first, for the
 * whole command: tristate is non-empty and holds only '0', '1' and
 * uppercase 'F' -- not yet checked against its length limit, so an
 * over-length tristate that also has an invalid character is still a
 * syntax error, not a range error. pulse follows the same digit,
 * no-leading-zero and 10-digit rules as a v2 field (field_ok()).
 * Only once both pass syntax is either range-checked -- tristate's
 * length against SOCKET_CODEWORD_MAX_LEN, then pulse's converted
 * value against [SOCKET_PULSE_LEN_MIN_US, SOCKET_PULSE_LEN_MAX_US] --
 * and only then is tristate copied into *cmd. Never builds an error
 * text from a byte of tristate or pulse.
 */
static void parse_raw(const char *tristate, size_t tlen, const char *pulse,
		       size_t plen, struct proto_cmd *cmd)
{
	size_t i;
	uint32_t pulse_len;

	if (tlen == 0) {
		cmd->kind = PROTO_ERROR;
		cmd->err = PROTO_ERR_SYNTAX;
		snprintf(cmd->text, sizeof(cmd->text), "malformed command");
		return;
	}
	for (i = 0; i < tlen; i++) {
		if (tristate[i] != '0' && tristate[i] != '1' &&
		    tristate[i] != 'F') {
			cmd->kind = PROTO_ERROR;
			cmd->err = PROTO_ERR_SYNTAX;
			snprintf(cmd->text, sizeof(cmd->text),
				 "malformed command");
			return;
		}
	}

	if (!field_ok(pulse, plen)) {
		cmd->kind = PROTO_ERROR;
		cmd->err = PROTO_ERR_SYNTAX;
		snprintf(cmd->text, sizeof(cmd->text), "malformed command");
		return;
	}

	if (tlen > SOCKET_CODEWORD_MAX_LEN) {
		cmd->kind = PROTO_ERROR;
		cmd->err = PROTO_ERR_RANGE;
		snprintf(cmd->text, sizeof(cmd->text),
			 "tristate longer than %u", SOCKET_CODEWORD_MAX_LEN);
		return;
	}

	if (field_to_u32(pulse, plen, &pulse_len) < 0) {
		cmd->kind = PROTO_ERROR;
		cmd->err = PROTO_ERR_RANGE;
		snprintf(cmd->text, sizeof(cmd->text), "pulse_len too large");
		return;
	}

	if (pulse_len < SOCKET_PULSE_LEN_MIN_US ||
	    pulse_len > SOCKET_PULSE_LEN_MAX_US) {
		cmd->kind = PROTO_ERROR;
		cmd->err = PROTO_ERR_RANGE;
		snprintf(cmd->text, sizeof(cmd->text), "pulse_len %" PRIu32
			 " not in %u-%u", pulse_len, SOCKET_PULSE_LEN_MIN_US,
			 SOCKET_PULSE_LEN_MAX_US);
		return;
	}

	cmd->kind = PROTO_RAW;
	memcpy(cmd->tristate, tristate, tlen);
	cmd->tristate[tlen] = '\0';
	cmd->pulse_len = pulse_len;
}

/*
 * Parses one already-delimited line (len > 0, no trailing \r or \n)
 * into *cmd. Exactly three fields whose first is the lowercase
 * keyword "raw" follow the raw grammar (parse_raw() above); every
 * other three-field line is "malformed command", same as any field
 * count other than 3 or 4. Otherwise: "Syntax before range, first
 * failing field wins" for the v2 grammar -- every field's syntax is
 * checked first; only once all four pass are they converted, in
 * field order (dev, group, socket, data), and the first one whose
 * value does not fit wins the RANGE error. Once every field converts,
 * check_range() applies the per-device bounds check.
 */
static void parse_line(const char *line, size_t len, struct proto_cmd *cmd)
{
	static const char *const names[4] = { "dev", "group", "socket",
					       "data" };
	const char *field[4];
	size_t flen[4];
	uint32_t val[4];
	size_t nfields;
	size_t i;

	nfields = split_fields(line, len, field, flen, 4);

	if (nfields == 3 && flen[0] == 3 && field[0][0] == 'r' &&
	    field[0][1] == 'a' && field[0][2] == 'w') {
		parse_raw(field[1], flen[1], field[2], flen[2], cmd);
		return;
	}

	if (nfields != 4) {
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

	check_range(cmd, val[0], val[1], val[2], val[3]);
}

void proto_init(struct proto_conn *pc)
{
	pc->len = 0;
	pc->discarding = 0;
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
