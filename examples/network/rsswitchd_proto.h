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
 * rsswitchd's wire protocol parser, kept in its own unit with no
 * socket, GPIO or syslog dependency, so it is unit-testable without a
 * daemon or hardware (PROTO-07).
 *
 * Contract summary (see the individual declarations below for the
 * exact rules):
 *
 *   - Everything works on byte counts, not C strings: input may
 *     contain NUL or any other byte, and is never assumed to be
 *     NUL-terminated. The only digits are the ASCII bytes '0'..'9',
 *     tested with an explicit range comparison -- never the
 *     locale-dependent isdigit().
 *   - A legacy command is exactly 4 buffered digits at a command
 *     boundary, complete as soon as the 4th digit arrives; no
 *     newline is needed. A \n, or \r immediately followed by \n,
 *     right after those 4 digits is swallowed silently. A lone \r
 *     waits for the next byte; anything other than a following \n
 *     starts the next command (with that \r as its first byte).
 *   - A line (anything else, up to the next \n) is bounded to
 *     PROTO_MAX_LINE bytes, not counting the \n itself but counting
 *     an embedded \r; exceeding it produces PROTO_ERR_TOOLONG at
 *     once and discards input through the next \n.
 *   - A non-legacy line is parsed as the strict protocol v2 grammar:
 *     exactly 4 fields, ASCII digits only, single-space separated,
 *     no leading zero unless the field is exactly "0". Syntax is
 *     checked for every field before any is converted; only once
 *     every field passes does conversion happen, in field order, so
 *     the first field whose value does not fit (more than 10 digits,
 *     or a value above UINT32_MAX) wins the PROTO_ERR_RANGE error.
 *   - Once dev, group, socket and data are all syntactically valid
 *     numbers (legacy or v2), they are checked in that same order
 *     against librsswitch.h's socket_addr_limits() (D-12, D-13) --
 *     never against a table copied into this file. The first field
 *     out of range wins: an unknown dev gives "dev %u unknown"; an
 *     out-of-range group, socket or data gives "<field> %u not in
 *     0-%u", naming the field's highest valid value. Only once every
 *     field passes this check does the result become PROTO_SWITCH.
 *   - A line of exactly three fields whose first field is exactly the
 *     three bytes "raw" (lowercase) is the raw grammar (D-08): "raw
 *     <tristate> <pulse_len>". Every other three-field line is a
 *     syntax error -- the "raw" keyword is not case-insensitive and
 *     has no shorter or longer form. Syntax is checked first, for the
 *     whole command: the tristate is non-empty and holds only '0',
 *     '1' and uppercase 'F'; pulse_len follows the same digit,
 *     no-leading-zero and 10-digit rules as a v2 field. Only once
 *     both pass syntax is either range-checked: a tristate longer
 *     than SOCKET_CODEWORD_MAX_LEN, checked before its length is used
 *     for anything else, or a pulse_len outside
 *     [SOCKET_PULSE_LEN_MIN_US, SOCKET_PULSE_LEN_MAX_US]. Passing
 *     both produces PROTO_RAW.
 *   - An empty line (bare \n or \r\n at a command boundary) is
 *     PROTO_EMPTY: not a command, no reply.
 *   - Error text is always built from fixed words and already-parsed
 *     numbers -- it never contains a byte from the input line.
 *   - There is no static or global state anywhere in this unit: all
 *     state lives in the caller-owned struct proto_conn, so any
 *     number of connections can be parsed independently, interleaved
 *     or not, with identical results for identical input.
 *
 * v2 forbids leading zeros (above); rsssend's command-line parser
 * deliberately accepts them (it is a CLI, not the wire protocol) --
 * do not reuse that idiom here unmodified.
 */

#ifndef RSSWITCHD_PROTO_H
#define RSSWITCHD_PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "../../librsswitch.h"

/*
 * Maximum accepted line length, in bytes, not counting the
 * terminating \n (a \r immediately before it does count). A byte
 * that would make the accumulated line exceed this aborts it at once
 * with PROTO_ERR_TOOLONG; input is then discarded up to and
 * including the next \n before parsing resumes.
 */
#define PROTO_MAX_LINE 128U

/* Size of struct proto_cmd's text field, including the terminating
 * NUL.
 */
#define PROTO_TEXT_LEN 64

/*
 * The result of one command boundary. PROTO_EMPTY is an ignored
 * empty line (D-09): no reply, nothing sent. PROTO_SWITCH is a fully
 * parsed, well-formed legacy or v2 switch command. PROTO_RAW is a
 * well-formed raw tri-state command (D-08). PROTO_ERROR is a
 * malformed, out-of-range or too-long line.
 */
enum proto_kind {
	PROTO_EMPTY,
	PROTO_SWITCH,
	PROTO_RAW,
	PROTO_ERROR
};

/*
 * The four error classes a reply's first word after "ERR " names
 * (D-11). PROTO_ERR_SEND is never produced by this parser -- it has
 * no socket_send() call to fail -- rsswitchd.c sets it after a
 * library call returns -1. proto_err_name() maps each class to its
 * wire/log word.
 */
enum proto_err {
	PROTO_ERR_SYNTAX,
	PROTO_ERR_RANGE,
	PROTO_ERR_TOOLONG,
	PROTO_ERR_SEND
};

/*
 * One parsed command boundary, filled in by proto_next().
 *
 * kind == PROTO_SWITCH: legacy is 1 for a 4-digit legacy command, 0
 * for a v2 line; dev/group/socket/data hold the parsed fields (D-05
 * for legacy, D-07 for v2), already checked against UINT32_MAX and
 * against socket_addr_limits()'s per-device bounds (D-12, D-13).
 * err/text are unused.
 *
 * kind == PROTO_RAW: tristate is a NUL-terminated string of '0', '1'
 * and uppercase 'F', 1 to SOCKET_CODEWORD_MAX_LEN bytes long, exactly
 * as given by the client; pulse_len holds the parsed pulse length,
 * already checked against [SOCKET_PULSE_LEN_MIN_US,
 * SOCKET_PULSE_LEN_MAX_US] (D-08). Every other field is unused.
 *
 * kind == PROTO_ERROR: err names the class and text holds a short,
 * human-readable explanation built only from fixed words and
 * already-parsed numbers (D-15) -- never from bytes in the input
 * line. legacy/dev/group/socket/data/tristate/pulse_len are unused.
 *
 * kind == PROTO_EMPTY: no other field is meaningful.
 */
struct proto_cmd {
	enum proto_kind kind;
	int legacy;
	uint32_t dev;
	uint32_t group;
	uint32_t socket;
	uint32_t data;
	char tristate[SOCKET_CODEWORD_MAX_LEN + 1];
	uint32_t pulse_len;
	enum proto_err err;
	char text[PROTO_TEXT_LEN];
};

/*
 * Per-connection parser state: the line accumulator (buf/len), plus
 * the two flags that carry across proto_next() calls -- discarding
 * (a too-long line is being skipped up to its next \n) and
 * legacy_tail (the last result was a legacy command whose
 * immediately following \n or \r\n must still be swallowed, D-02).
 * There is no state anywhere else: zero-initialize or call
 * proto_init() before first use, and any number of struct proto_conn
 * can be driven independently with no interaction between them.
 */
struct proto_conn {
	char buf[PROTO_MAX_LINE + 1];
	size_t len;
	int discarding;
	int legacy_tail;
};

/*
 * Initializes *pc to the empty, non-discarding, non-legacy-tail
 * state. Safe to call again on a *pc already in use, to reset a
 * connection.
 */
void proto_init(struct proto_conn *pc);

/*
 * Consumes bytes from *in for up to *inlen bytes, advancing *in and
 * decreasing *inlen by however much it consumed. *in points at a raw
 * byte stream -- it may contain NUL or any other byte, and is never
 * assumed to be NUL-terminated.
 *
 * As soon as *cmd holds one complete result (an empty line, a parsed
 * command or a parse error), returns 1 and stops, leaving any
 * remaining bytes of *in unconsumed for the caller's next call.
 * Returns 0 once every byte of *in has been consumed without a
 * complete result yet -- a partial line, if any, stays buffered in
 * *pc for the next call. Called with *inlen == 0, always returns 0
 * with *pc unchanged.
 *
 * Never allocates, never blocks, and touches nothing beyond its own
 * arguments and *pc's state -- no socket, board or log dependency of
 * any kind.
 */
int proto_next(struct proto_conn *pc, const char **in, size_t *inlen,
	       struct proto_cmd *cmd);

/*
 * Maps a proto_err to its fixed wire/log word: "syntax", "range",
 * "toolong" or "send". Used for both the ERR reply's first word and
 * the syslog error line (D-11).
 */
const char *proto_err_name(enum proto_err err);

#endif /* RSSWITCHD_PROTO_H */
