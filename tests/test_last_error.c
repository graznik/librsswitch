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
 * Tests rss_last_error(): the cause of a failed send, how the
 * library turns what a board returns into that cause, that a success
 * clears it, and that it is kept per thread.
 *
 * Includes ../librsswitch.c directly, instead of linking it, so it can
 * reach the static cause_from_board(). It defines the five board
 * functions itself as a settable test board, so the cause a board
 * returns can be chosen per test without the x86_64 stub, and links no
 * board source.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "minitest.h"
#include "../librsswitch.c"

/* What board_init() returns, and how often it was called. */
static int32_t test_board_init_ret;
static size_t test_board_init_calls;

/*
 * transmit() returns 0 for its first test_board_transmit_fail_at
 * calls, and test_board_transmit_ret from then on.
 */
static int32_t test_board_transmit_ret;
static size_t test_board_transmit_fail_at;
static size_t test_board_transmit_calls;

/*
 * What board_codeword() saw: how often it was called, how many
 * transmit() calls had happened by then, and the codeword.
 */
static size_t test_board_codeword_calls;
static size_t test_board_codeword_transmits;
static char test_board_codeword_last[MAX_LEN];

int32_t board_init(const char *config)
{
	(void)config;
	test_board_init_calls++;
	return test_board_init_ret;
}

int32_t board_config_check(const char *config)
{
	(void)config;
	return 0;
}

int32_t board_exit(void)
{
	return 0;
}

int32_t transmit(uint32_t nhigh, uint32_t nlow, uint32_t pulse_len)
{
	(void)nhigh;
	(void)nlow;
	(void)pulse_len;
	if (test_board_transmit_calls++ < test_board_transmit_fail_at)
		return 0;
	return test_board_transmit_ret;
}

void board_codeword(const char *codeword)
{
	test_board_codeword_calls++;
	test_board_codeword_transmits = test_board_transmit_calls;
	snprintf(test_board_codeword_last, sizeof(test_board_codeword_last),
		 "%s", codeword);
}

static void reset_board(void)
{
	test_board_init_ret = 0;
	test_board_init_calls = 0;
	test_board_transmit_ret = 0;
	test_board_transmit_fail_at = 0;
	test_board_transmit_calls = 0;
	test_board_codeword_calls = 0;
	test_board_codeword_transmits = 0;
	test_board_codeword_last[0] = '\0';
}

/* One send through each public entry point, with a valid address. */
static int32_t send_valid(void)
{
	return rss_send(RSS_POLLIN_2605, 17, 23, 1);
}

static int32_t send_valid_codeword(void)
{
	return rss_send_codeword("1F0", 350);
}

/* Leaves the given cause in the calling thread, then resets the board. */
static void prime_cause(int32_t board_ret)
{
	reset_board();
	test_board_init_ret = board_ret;
	CHECK_EQ(send_valid(), -1);
	reset_board();
}

static void test_board_cause_reaches_caller(void)
{
	static const struct {
		int32_t board_ret;
		enum rss_err cause;
	} table[] = {
		{-1, RSS_ERR_OTHER},
		{-2, RSS_ERR_NOCHIP},
		{-3, RSS_ERR_BUSY},
		{-4, RSS_ERR_DENIED},
		{-5, RSS_ERR_BADLINE},
		{-6, RSS_ERR_IO},
		{-7, RSS_ERR_OTHER},
		{-100, RSS_ERR_OTHER},
		{INT32_MIN, RSS_ERR_OTHER},
	};
	size_t i;

	reset_board();
	CHECK_EQ(rss_last_error(), RSS_ERR_NONE);

	CHECK_EQ(cause_from_board(0), RSS_ERR_NONE);
	CHECK_EQ(cause_from_board(INT32_MIN), RSS_ERR_OTHER);

	for (i = 0; i < ARRAY_SIZE(table); i++) {
		reset_board();
		test_board_init_ret = table[i].board_ret;
		CHECK_EQ(cause_from_board(table[i].board_ret),
			 table[i].cause);
		CHECK_EQ(send_valid(), -1);
		CHECK_EQ(rss_last_error(), table[i].cause);

		prime_cause(-3);
		test_board_init_ret = table[i].board_ret;
		CHECK_EQ(send_valid_codeword(), -1);
		CHECK_EQ(rss_last_error(), table[i].cause);
	}
}

static void test_transmit_cause_reaches_caller(void)
{
	/* The first pulse fails. */
	reset_board();
	test_board_transmit_ret = -(int32_t)RSS_ERR_IO;
	CHECK_EQ(send_valid(), -1);
	CHECK_EQ(rss_last_error(), RSS_ERR_IO);

	reset_board();
	test_board_transmit_ret = -(int32_t)RSS_ERR_IO;
	CHECK_EQ(send_valid_codeword(), -1);
	CHECK_EQ(rss_last_error(), RSS_ERR_IO);

	/* The second pulse of the first symbol fails. */
	reset_board();
	test_board_transmit_ret = -(int32_t)RSS_ERR_BUSY;
	test_board_transmit_fail_at = 1;
	CHECK_EQ(send_valid_codeword(), -1);
	CHECK_EQ(rss_last_error(), RSS_ERR_BUSY);

	/* The sync pulse of the first repetition of "1" fails. */
	reset_board();
	test_board_transmit_ret = -(int32_t)RSS_ERR_BUSY;
	test_board_transmit_fail_at = 2;
	CHECK_EQ(rss_send_codeword("1", 350), -1);
	CHECK_EQ(rss_last_error(), RSS_ERR_BUSY);

	/* A later repetition of a whole address fails. */
	reset_board();
	test_board_transmit_ret = -(int32_t)RSS_ERR_BUSY;
	test_board_transmit_fail_at = 100;
	CHECK_EQ(send_valid(), -1);
	CHECK_EQ(rss_last_error(), RSS_ERR_BUSY);

	/* A plain -1 from transmit() is "other". */
	reset_board();
	test_board_transmit_ret = -1;
	CHECK_EQ(send_valid(), -1);
	CHECK_EQ(rss_last_error(), RSS_ERR_OTHER);
}

static void test_success_clears_cause(void)
{
	prime_cause(-(int32_t)RSS_ERR_BUSY);
	CHECK_EQ(rss_last_error(), RSS_ERR_BUSY);
	CHECK_EQ(send_valid(), 0);
	CHECK_EQ(rss_last_error(), RSS_ERR_NONE);

	prime_cause(-(int32_t)RSS_ERR_DENIED);
	CHECK_EQ(rss_last_error(), RSS_ERR_DENIED);
	CHECK_EQ(send_valid_codeword(), 0);
	CHECK_EQ(rss_last_error(), RSS_ERR_NONE);
}

/* A rejected call is -1 with ARG and never touches the board. */
static void check_rejected(int32_t ret)
{
	CHECK_EQ(ret, -1);
	CHECK_EQ(rss_last_error(), RSS_ERR_ARG);
	CHECK_EQ(test_board_init_calls, (size_t)0);
	CHECK_EQ(test_board_codeword_calls, (size_t)0);
}

static void test_rejected_arguments_are_arg(void)
{
	/*
	 * Every rejection starts from a stale cause that is not ARG, so
	 * the result shows the call set it.
	 */
	prime_cause(-(int32_t)RSS_ERR_BUSY);
	check_rejected(rss_send(9, 0, 0, 0));

	prime_cause(-(int32_t)RSS_ERR_BUSY);
	check_rejected(rss_send(RSS_POLLIN_2605, 32, 0, 0));

	prime_cause(-(int32_t)RSS_ERR_BUSY);
	check_rejected(rss_send(RSS_POLLIN_2605, 0, 32, 0));

	prime_cause(-(int32_t)RSS_ERR_BUSY);
	check_rejected(rss_send(RSS_POLLIN_2605, 0, 0, 2));

	prime_cause(-(int32_t)RSS_ERR_BUSY);
	check_rejected(rss_send_codeword(NULL, 350));

	prime_cause(-(int32_t)RSS_ERR_BUSY);
	check_rejected(rss_send_codeword("", 350));

	prime_cause(-(int32_t)RSS_ERR_BUSY);
	check_rejected(rss_send_codeword("0123456789ABC", 350));

	prime_cause(-(int32_t)RSS_ERR_BUSY);
	check_rejected(rss_send_codeword("0f", 350));

	prime_cause(-(int32_t)RSS_ERR_BUSY);
	check_rejected(rss_send_codeword("0F1",
					 RSS_PULSE_LEN_MIN_US - 1));

	prime_cause(-(int32_t)RSS_ERR_BUSY);
	check_rejected(rss_send_codeword("0F1",
					 RSS_PULSE_LEN_MAX_US + 1));
}

/*
 * A send whose board_init() succeeded hands the board its codeword
 * once, before the first transmit(); a send that fails earlier does
 * not.
 */
static void test_board_gets_codeword_once(void)
{
	reset_board();
	CHECK_EQ(send_valid_codeword(), 0);
	CHECK_EQ(test_board_codeword_calls, (size_t)1);
	CHECK_EQ(test_board_codeword_transmits, (size_t)0);
	CHECK_EQ(strcmp(test_board_codeword_last, "1F0"), 0);
	CHECK(test_board_transmit_calls > 0);

	reset_board();
	CHECK_EQ(send_valid(), 0);
	CHECK_EQ(test_board_codeword_calls, (size_t)1);
	CHECK_EQ(test_board_codeword_transmits, (size_t)0);
	CHECK_EQ(tristate_check(test_board_codeword_last), 0);
	CHECK(test_board_transmit_calls > 0);

	/* A failing transmit() does not repeat or withdraw it. */
	reset_board();
	test_board_transmit_ret = -(int32_t)RSS_ERR_IO;
	CHECK_EQ(send_valid_codeword(), -1);
	CHECK_EQ(test_board_codeword_calls, (size_t)1);
	CHECK_EQ(test_board_codeword_transmits, (size_t)0);

	reset_board();
	test_board_transmit_ret = -(int32_t)RSS_ERR_IO;
	CHECK_EQ(send_valid(), -1);
	CHECK_EQ(test_board_codeword_calls, (size_t)1);
	CHECK_EQ(test_board_codeword_transmits, (size_t)0);

	/* A failing board_init() sends nothing to announce. */
	reset_board();
	test_board_init_ret = -(int32_t)RSS_ERR_BUSY;
	CHECK_EQ(send_valid_codeword(), -1);
	CHECK_EQ(send_valid(), -1);
	CHECK_EQ(test_board_codeword_calls, (size_t)0);
}

/* What a worker thread saw: its send result and its own cause. */
struct worker_ctx {
	int32_t ret;
	enum rss_err cause;
};

static void *worker_send(void *arg)
{
	struct worker_ctx *ctx = arg;

	ctx->ret = send_valid();
	ctx->cause = rss_last_error();
	return NULL;
}

static void run_worker(struct worker_ctx *ctx)
{
	pthread_t tid;

	ctx->ret = 1;
	ctx->cause = RSS_ERR_ARG;
	CHECK_EQ(pthread_create(&tid, NULL, worker_send, ctx), 0);
	CHECK_EQ(pthread_join(tid, NULL), 0);
}

static void test_cause_is_per_thread(void)
{
	struct worker_ctx ctx;

	/* The main thread succeeds, a worker fails. */
	reset_board();
	CHECK_EQ(send_valid(), 0);
	CHECK_EQ(rss_last_error(), RSS_ERR_NONE);

	test_board_init_ret = -(int32_t)RSS_ERR_BUSY;
	run_worker(&ctx);
	CHECK_EQ(ctx.ret, -1);
	CHECK_EQ(ctx.cause, RSS_ERR_BUSY);
	CHECK_EQ(rss_last_error(), RSS_ERR_NONE);

	/* The main thread fails, a worker succeeds. */
	reset_board();
	test_board_init_ret = -(int32_t)RSS_ERR_DENIED;
	CHECK_EQ(send_valid(), -1);
	CHECK_EQ(rss_last_error(), RSS_ERR_DENIED);

	test_board_init_ret = 0;
	run_worker(&ctx);
	CHECK_EQ(ctx.ret, 0);
	CHECK_EQ(ctx.cause, RSS_ERR_NONE);
	CHECK_EQ(rss_last_error(), RSS_ERR_DENIED);
}

static void test_other_calls_keep_cause(void)
{
	uint32_t ngroups, nsockets, ndata;

	prime_cause(-(int32_t)RSS_ERR_BUSY);
	CHECK_EQ(rss_last_error(), RSS_ERR_BUSY);

	CHECK_EQ(rss_addr_limits(RSS_POLLIN_2605, &ngroups, &nsockets, &ndata),
		 0);
	CHECK_EQ(rss_addr_limits(9, &ngroups, &nsockets, &ndata), -1);
	CHECK_EQ(rss_last_error(), RSS_ERR_BUSY);

	CHECK_EQ(rss_gpio_config_check(NULL), 0);
	CHECK_EQ(rss_last_error(), RSS_ERR_BUSY);

	rss_gpio_config(NULL);
	CHECK_EQ(rss_last_error(), RSS_ERR_BUSY);
}

int main(void)
{
	test_board_cause_reaches_caller();
	test_transmit_cause_reaches_caller();
	test_success_clears_cause();
	test_rejected_arguments_are_arg();
	test_board_gets_codeword_once();
	test_cause_is_per_thread();
	test_other_calls_keep_cause();

	return MT_REPORT();
}
