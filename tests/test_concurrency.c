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
#include <pthread.h>
#include <string.h>

#include "minitest.h"
#include "pulse_decode.h"
#include "../librsswitch.h"
#include "../boards/x86_64/x86_64.h"

#define NTHREADS 6
#define NSENDS 10
#define NROUNDS 4

/* group = 5t+1, socket = 31-4t, data = t%2, for t = 0..NTHREADS-1 */
static const uint32_t addr_group[NTHREADS] = {1, 6, 11, 16, 21, 26};
static const uint32_t addr_socket[NTHREADS] = {31, 27, 23, 19, 15, 11};
static const uint32_t addr_data[NTHREADS] = {0, 1, 0, 1, 0, 1};

static char ref[NTHREADS][PULSE_DECODE_MAX_FRAME_LEN];

struct worker_ctx {
	uint32_t group;
	uint32_t socket;
	uint32_t data;
	int is_thread0;
	int ok;
};

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

/*
 * Records one reference codeword per thread address, single-threaded,
 * by sending once and decoding the result with next_transmission().
 * Every round below then only needs to compare against these.
 */
static void compute_references(void)
{
	int t;

	for (t = 0; t < NTHREADS; t++) {
		size_t pos = 0;
		uint32_t pulse_len;

		x86_64_recorder_reset();
		CHECK_EQ(socket_send(POLLIN_2605, addr_group[t],
				      addr_socket[t], addr_data[t]), 0);
		CHECK_EQ(next_transmission(&pos, ref[t], sizeof(ref[t]),
					    &pulse_len), 0);
		CHECK_EQ(pulse_len, (uint32_t)350);
	}
}

static void *worker_main(void *arg)
{
	struct worker_ctx *ctx = arg;
	int i;

	for (i = 0; i < NSENDS; i++) {
		if (socket_send(POLLIN_2605, ctx->group, ctx->socket,
				ctx->data) != 0)
			ctx->ok = 0;

		/* Thread 0 also sends a rejected address between sends. */
		if (ctx->is_thread0 && i == 4) {
			if (socket_send(POLLIN_2605, 32, 0, 0) != -1)
				ctx->ok = 0;
		}
	}

	return NULL;
}

/*
 * One round: NTHREADS threads each send their own address NSENDS
 * times through socket_send() (thread 0 also sends one rejected
 * address in between); after joining, the whole recording must
 * decode into whole, non-interleaved NTIMES-frame transmissions, each
 * matching exactly one thread's reference codeword exactly NSENDS
 * times, with no overlap and a matching board_init()/board_exit()
 * count.
 *
 * NTHREADS * NSENDS + 1 counts every socket_send() call made this
 * round, including thread 0's one rejected address: board_init() and
 * board_exit() run unconditionally for every socket_send() call
 * (accepted or rejected) in the current, unchanged control flow --
 * only the codeword generation in between is skipped for a rejected
 * address.
 */
static void run_round(void)
{
	pthread_t tid[NTHREADS];
	struct worker_ctx ctx[NTHREADS];
	size_t counts[NTHREADS];
	size_t pos;
	int t;

	x86_64_recorder_reset();

	for (t = 0; t < NTHREADS; t++) {
		ctx[t].group = addr_group[t];
		ctx[t].socket = addr_socket[t];
		ctx[t].data = addr_data[t];
		ctx[t].is_thread0 = (t == 0);
		ctx[t].ok = 1;
		counts[t] = 0;
	}

	for (t = 0; t < NTHREADS; t++)
		CHECK_EQ(pthread_create(&tid[t], NULL, worker_main, &ctx[t]), 0);

	for (t = 0; t < NTHREADS; t++)
		CHECK_EQ(pthread_join(tid[t], NULL), 0);

	for (t = 0; t < NTHREADS; t++)
		CHECK(ctx[t].ok);

	pos = 0;
	while (pos < x86_64_recorder_count()) {
		char frame[PULSE_DECODE_MAX_FRAME_LEN];
		uint32_t pulse_len;
		int matched = -1;
		int decoded = (next_transmission(&pos, frame, sizeof(frame),
						  &pulse_len) == 0);

		CHECK(decoded);
		if (!decoded)
			break;

		CHECK_EQ(pulse_len, (uint32_t)350);

		for (t = 0; t < NTHREADS; t++) {
			if (strcmp(frame, ref[t]) == 0) {
				matched = t;
				break;
			}
		}
		CHECK(matched >= 0);
		if (matched >= 0)
			counts[matched]++;
	}

	CHECK_EQ(pos, x86_64_recorder_count());

	for (t = 0; t < NTHREADS; t++)
		CHECK_EQ(counts[t], (size_t)NSENDS);

	CHECK(!x86_64_recorder_overflowed());
	CHECK_EQ(x86_64_board_overlap_count(), (size_t)0);
	CHECK_EQ(x86_64_board_init_count(), (size_t)(NTHREADS * NSENDS + 1));
	CHECK_EQ(x86_64_board_exit_count(), (size_t)(NTHREADS * NSENDS + 1));
}

static void test_concurrent_socket_send(void)
{
	int round;

	compute_references();

	for (round = 0; round < NROUNDS; round++)
		run_round();
}

#define NTHREADS_MIXED 6
#define NSENDS_MIXED 10
#define NROUNDS_MIXED 4

static const uint32_t mixed_codeword_pulse_len[3] = {300, 320, 340};
static const char * const mixed_codewords[3] = {
	"0101FFFF0000", "1F1F1F1F1F1F", "0F0",
};

static char mixed_ref[NTHREADS_MIXED][PULSE_DECODE_MAX_FRAME_LEN];
static uint32_t mixed_ref_pulse_len[NTHREADS_MIXED];

struct mixed_worker_ctx {
	int use_codeword;	/* 1: socket_send_codeword(), 0: socket_send() */
	uint32_t group;
	uint32_t socket;
	uint32_t data;
	const char *codeword;
	uint32_t pulse_len;
	int is_thread0;
	int ok;
};

/*
 * Records one reference (codeword, pulse_len) pair per mixed-sender
 * thread: the first three via socket_send() (decoded, like
 * compute_references()), the last three the literal codewords sent
 * through socket_send_codeword().
 */
static void compute_mixed_references(void)
{
	int t;

	for (t = 0; t < 3; t++) {
		size_t pos = 0;

		x86_64_recorder_reset();
		CHECK_EQ(socket_send(POLLIN_2605, addr_group[t],
				      addr_socket[t], addr_data[t]), 0);
		CHECK_EQ(next_transmission(&pos, mixed_ref[t],
					    sizeof(mixed_ref[t]),
					    &mixed_ref_pulse_len[t]), 0);
		CHECK_EQ(mixed_ref_pulse_len[t], (uint32_t)350);
	}

	for (t = 0; t < 3; t++) {
		snprintf(mixed_ref[3 + t], sizeof(mixed_ref[3 + t]), "%s",
			 mixed_codewords[t]);
		mixed_ref_pulse_len[3 + t] = mixed_codeword_pulse_len[t];
	}
}

static void *mixed_worker_main(void *arg)
{
	struct mixed_worker_ctx *ctx = arg;
	int i;

	for (i = 0; i < NSENDS_MIXED; i++) {
		int32_t ret;

		if (ctx->use_codeword)
			ret = socket_send_codeword(ctx->codeword,
						    ctx->pulse_len);
		else
			ret = socket_send(POLLIN_2605, ctx->group,
					   ctx->socket, ctx->data);

		if (ret != 0)
			ctx->ok = 0;

		/* Thread 0 also sends a rejected address between sends. */
		if (ctx->is_thread0 && i == 4) {
			if (socket_send(POLLIN_2605, 32, 0, 0) != -1)
				ctx->ok = 0;
		}
	}

	return NULL;
}

/*
 * Like run_round(), but three threads send through socket_send() and
 * three through socket_send_codeword() with distinct literal
 * codewords and distinct pulse_len values (D-09: different codewords
 * through both entry points), proving the two entry points share the
 * same lock and never interleave with each other either.
 */
static void run_mixed_round(void)
{
	pthread_t tid[NTHREADS_MIXED];
	struct mixed_worker_ctx ctx[NTHREADS_MIXED];
	size_t counts[NTHREADS_MIXED];
	size_t pos;
	int t;

	x86_64_recorder_reset();

	for (t = 0; t < 3; t++) {
		ctx[t].use_codeword = 0;
		ctx[t].group = addr_group[t];
		ctx[t].socket = addr_socket[t];
		ctx[t].data = addr_data[t];
		ctx[t].codeword = NULL;
		ctx[t].pulse_len = 0;
		ctx[t].is_thread0 = (t == 0);
		ctx[t].ok = 1;
		counts[t] = 0;
	}
	for (t = 0; t < 3; t++) {
		ctx[3 + t].use_codeword = 1;
		ctx[3 + t].group = 0;
		ctx[3 + t].socket = 0;
		ctx[3 + t].data = 0;
		ctx[3 + t].codeword = mixed_codewords[t];
		ctx[3 + t].pulse_len = mixed_codeword_pulse_len[t];
		ctx[3 + t].is_thread0 = 0;
		ctx[3 + t].ok = 1;
		counts[3 + t] = 0;
	}

	for (t = 0; t < NTHREADS_MIXED; t++)
		CHECK_EQ(pthread_create(&tid[t], NULL, mixed_worker_main,
					 &ctx[t]), 0);

	for (t = 0; t < NTHREADS_MIXED; t++)
		CHECK_EQ(pthread_join(tid[t], NULL), 0);

	for (t = 0; t < NTHREADS_MIXED; t++)
		CHECK(ctx[t].ok);

	pos = 0;
	while (pos < x86_64_recorder_count()) {
		char frame[PULSE_DECODE_MAX_FRAME_LEN];
		uint32_t pulse_len;
		int matched = -1;
		int decoded = (next_transmission(&pos, frame, sizeof(frame),
						  &pulse_len) == 0);

		CHECK(decoded);
		if (!decoded)
			break;

		for (t = 0; t < NTHREADS_MIXED; t++) {
			if (pulse_len == mixed_ref_pulse_len[t] &&
			    strcmp(frame, mixed_ref[t]) == 0) {
				matched = t;
				break;
			}
		}
		CHECK(matched >= 0);
		if (matched >= 0)
			counts[matched]++;
	}

	CHECK_EQ(pos, x86_64_recorder_count());

	for (t = 0; t < NTHREADS_MIXED; t++)
		CHECK_EQ(counts[t], (size_t)NSENDS_MIXED);

	CHECK(!x86_64_recorder_overflowed());
	CHECK_EQ(x86_64_board_overlap_count(), (size_t)0);
	CHECK_EQ(x86_64_board_init_count(),
		 (size_t)(NTHREADS_MIXED * NSENDS_MIXED + 1));
	CHECK_EQ(x86_64_board_exit_count(),
		 (size_t)(NTHREADS_MIXED * NSENDS_MIXED + 1));
}

static void test_concurrent_mixed_senders(void)
{
	int round;

	compute_mixed_references();

	for (round = 0; round < NROUNDS_MIXED; round++)
		run_mixed_round();
}

int main(void)
{
	test_overlap_detector_is_not_vacuous();
	test_concurrent_socket_send();
	test_concurrent_mixed_senders();

	return MT_REPORT();
}
