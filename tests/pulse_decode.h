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
 * Decodes pulses recorded by the x86_64 board's test-only recorder
 * back into the 0/1/F/sync tri-state frames librsswitch.c's
 * send_0()/send_1()/send_f()/send_sync() produce. Used only by
 * tests/test_*.c to assert on the exact codeword a socket_send() (or,
 * later, socket_send_codeword()) call transmitted.
 */
#ifndef PULSE_DECODE_H
#define PULSE_DECODE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../boards/x86_64/x86_64.h"

/*
 * Literal on purpose: if NTIMES in librsswitch.c ever changes, this
 * mismatch must fail the test suite rather than silently decode a
 * different repeat count as correct.
 */
#define EXPECTED_REPEATS 10

/* Longest codeword this project supports (PT2260/PT2262 word length). */
#define PULSE_DECODE_MAX_FRAME_LEN 32

/*
 * Decodes a single frame starting at *pos: a sequence of tri-state
 * symbols ('0'/'1'/'F'), each encoded as two pulses, followed by
 * exactly one sync pulse (1, 31). On success, writes the
 * NUL-terminated decoded frame to out, advances *pos past the sync
 * pulse, records the frame's common pulse_len in *pulse_len and
 * returns 0.
 *
 * Returns -1 on any of: an unrecognized pulse shape, a pulse_len that
 * differs from the rest of the frame, a missing sync pulse (recorder
 * runs out before one is found), or an output buffer too small to
 * hold the decoded symbols.
 */
static inline int decode_frame(size_t *pos, char *out, size_t outlen,
				uint32_t *pulse_len)
{
	size_t i = *pos;
	size_t olen = 0;
	int have_pulse_len = 0;

	for (;;) {
		const struct x86_64_pulse *p = x86_64_recorder_get(i);
		const struct x86_64_pulse *q;
		char sym;

		if (!p)
			return -1;

		if (!have_pulse_len) {
			*pulse_len = p->pulse_len;
			have_pulse_len = 1;
		} else if (p->pulse_len != *pulse_len) {
			return -1;
		}

		if (p->nhigh == 1 && p->nlow == 31) {
			if (olen >= outlen)
				return -1;
			out[olen] = '\0';
			*pos = i + 1;
			return 0;
		}

		q = x86_64_recorder_get(i + 1);
		if (!q || q->pulse_len != *pulse_len)
			return -1;

		if (p->nhigh == 1 && p->nlow == 3 &&
		    q->nhigh == 1 && q->nlow == 3)
			sym = '0';
		else if (p->nhigh == 3 && p->nlow == 1 &&
			 q->nhigh == 3 && q->nlow == 1)
			sym = '1';
		else if (p->nhigh == 1 && p->nlow == 3 &&
			 q->nhigh == 3 && q->nlow == 1)
			sym = 'F';
		else
			return -1;

		if (olen + 1 >= outlen)
			return -1;
		out[olen++] = sym;
		i += 2;
	}
}

/*
 * Decodes EXPECTED_REPEATS consecutive frames starting at *pos. Every
 * frame must decode to the same content and the same pulse_len as the
 * first one. On success, out holds the shared decoded content, *pos
 * has advanced past all EXPECTED_REPEATS frames and *pulse_len holds
 * the shared pulse_len. Returns 0 on success, -1 otherwise.
 */
static inline int next_transmission(size_t *pos, char *out, size_t outlen,
				     uint32_t *pulse_len)
{
	char frame[PULSE_DECODE_MAX_FRAME_LEN];
	uint32_t frame_pulse_len;
	size_t n;

	if (decode_frame(pos, out, outlen, pulse_len) < 0)
		return -1;

	for (n = 1; n < EXPECTED_REPEATS; n++) {
		if (decode_frame(pos, frame, sizeof(frame),
				  &frame_pulse_len) < 0)
			return -1;
		if (frame_pulse_len != *pulse_len)
			return -1;
		if (strcmp(frame, out) != 0)
			return -1;
	}

	return 0;
}

/*
 * Asserts that the recorder currently holds exactly one transmission
 * (EXPECTED_REPEATS identical frames, all at pulse_len, and nothing
 * else) of codeword, and that the overflow flag is clear. Returns 0
 * if so, -1 otherwise.
 */
static inline int expect_transmission(const char *codeword,
				       uint32_t pulse_len)
{
	char decoded[PULSE_DECODE_MAX_FRAME_LEN];
	uint32_t decoded_pulse_len;
	size_t pos = 0;
	size_t expected_count;

	if (!codeword)
		return -1;

	if (x86_64_recorder_overflowed())
		return -1;

	expected_count = (size_t)EXPECTED_REPEATS *
			 (2 * strlen(codeword) + 1);
	if (x86_64_recorder_count() != expected_count)
		return -1;

	if (next_transmission(&pos, decoded, sizeof(decoded),
			       &decoded_pulse_len) < 0)
		return -1;

	if (decoded_pulse_len != pulse_len)
		return -1;

	if (strcmp(decoded, codeword) != 0)
		return -1;

	if (pos != x86_64_recorder_count())
		return -1;

	return 0;
}

#endif /* PULSE_DECODE_H */
