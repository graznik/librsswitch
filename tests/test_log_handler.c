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
 * Tests rss_set_log_handler(): a handler gets every message of the
 * library and its boards as finished text, with the priority the
 * default handler would give to syslog() and the context it was set
 * with; NULL restores the default; a long message is cut, not
 * overflowed; the function and context are never seen torn apart.
 *
 * syslog is redefined to capture_syslog before ../librsswitch.c is
 * included, as tests/test_logging.c does, so the default handler's
 * output is captured instead of going to the real syslog. The file is
 * built with -DDEBUG (see tests/Makefile), so the notice that rss_send()
 * logs for every call is among the messages too. The messages of the
 * "chip:line" parser come from the stub library's archive, which calls
 * the board_log() of the included file.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#undef syslog
#define syslog capture_syslog

static void capture_syslog(int priority, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

#include "minitest.h"
#include "../librsswitch.c"

#define MAX_MESSAGES 16
#define MSG_LEN 300

/* What a logger saw: the default handler's syslog() calls or a handler. */
struct record {
	size_t calls;
	int priority[MAX_MESSAGES];
	char text[MAX_MESSAGES][MSG_LEN];
	size_t len[MAX_MESSAGES];
	pthread_t thread[MAX_MESSAGES];
};

static struct record captured;

static void record_message(struct record *r, int priority, const char *msg)
{
	size_t i = r->calls++;

	if (i >= MAX_MESSAGES)
		return;

	r->priority[i] = priority;
	snprintf(r->text[i], sizeof(r->text[i]), "%s", msg);
	r->len[i] = strlen(msg);
	r->thread[i] = pthread_self();
}

/* The stand-in for syslog(): the default handler formats "%s". */
static void capture_syslog(int priority, const char *fmt, ...)
{
	char msg[MSG_LEN];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	record_message(&captured, priority, msg);
}

/* A handler that records into the struct record its ctx points to. */
static void recording_handler(int priority, const char *msg, void *ctx)
{
	record_message(ctx, priority, msg);
}

static void reset(struct record *r)
{
	memset(r, 0, sizeof(*r));
}

static void restore_default(void)
{
	rss_set_log_handler(NULL, NULL);
	reset(&captured);
}

/*
 * With no handler set, an invalid call reaches syslog(): the notice
 * the DEBUG build logs for every call, then the error.
 */
static void test_default_goes_to_syslog(void)
{
	restore_default();

	CHECK_EQ(rss_send(5, 0, 0, 0), -1);
	CHECK_EQ(captured.calls, (size_t)2);
	CHECK_EQ(captured.priority[0], LOG_NOTICE);
	CHECK_EQ(strcmp(captured.text[0],
			"dev: 5, group: 0, socket: 0, data: 0"), 0);
	CHECK_EQ(captured.priority[1], LOG_ERR);
	CHECK_EQ(strcmp(captured.text[1], "[-] Unknown socket type 5"), 0);
}

/*
 * A handler gets the same priorities and texts the default handler
 * passes to syslog(), the context it was set with, in the calling
 * thread; syslog() sees nothing.
 */
static void test_handler_gets_what_syslog_got(void)
{
	static struct record rec, def;
	size_t i;

	restore_default();
	CHECK_EQ(rss_send(5, 0, 0, 0), -1);
	def = captured;
	CHECK_EQ(def.calls, (size_t)2);

	reset(&rec);
	rss_set_log_handler(recording_handler, &rec);
	reset(&captured);
	CHECK_EQ(rss_send(5, 0, 0, 0), -1);

	CHECK_EQ(captured.calls, (size_t)0);
	CHECK_EQ(rec.calls, (size_t)2);
	for (i = 0; i < rec.calls && i < MAX_MESSAGES; i++) {
		CHECK(pthread_equal(rec.thread[i], pthread_self()));
		CHECK(strlen(rec.text[i]) == rec.len[i]);
		CHECK_EQ(rec.priority[i], def.priority[i]);
		CHECK_EQ(strcmp(rec.text[i], def.text[i]), 0);
	}
	CHECK_EQ(rec.priority[0], LOG_NOTICE);
	CHECK_EQ(strcmp(rec.text[0], "dev: 5, group: 0, socket: 0, data: 0"),
		 0);
	CHECK_EQ(rec.priority[1], LOG_ERR);
	CHECK_EQ(strcmp(rec.text[1], "[-] Unknown socket type 5"), 0);

	restore_default();
}

/* The context given last is the one the handler gets. */
static void test_context_is_passed_through(void)
{
	static struct record rec1, rec2;

	reset(&rec1);
	reset(&rec2);

	rss_set_log_handler(recording_handler, &rec1);
	CHECK_EQ(rss_send(5, 0, 0, 0), -1);
	CHECK_EQ(rec1.calls, (size_t)2);
	CHECK_EQ(rec2.calls, (size_t)0);

	rss_set_log_handler(recording_handler, &rec2);
	CHECK_EQ(rss_send(5, 0, 0, 0), -1);
	CHECK_EQ(rec1.calls, (size_t)2);
	CHECK_EQ(rec2.calls, (size_t)2);

	restore_default();
}

/*
 * NULL restores the default: the messages reach syslog() again and
 * the handler is not called again.
 */
static void test_null_restores_default(void)
{
	static struct record rec;

	reset(&rec);
	rss_set_log_handler(recording_handler, &rec);
	CHECK_EQ(rss_send(5, 0, 0, 0), -1);
	CHECK_EQ(rec.calls, (size_t)2);

	rss_set_log_handler(NULL, &rec);
	reset(&captured);
	CHECK_EQ(rss_send(5, 0, 0, 0), -1);
	CHECK_EQ(rec.calls, (size_t)2);
	CHECK_EQ(captured.calls, (size_t)2);
	CHECK_EQ(strcmp(captured.text[1], "[-] Unknown socket type 5"), 0);

	restore_default();
}

/*
 * The message of a board comes through the handler too: the parser
 * of the "chip:line" grammar logs from outside this file.
 */
static void test_board_message_reaches_handler(void)
{
	static struct record rec;

	reset(&rec);
	rss_set_log_handler(recording_handler, &rec);

	CHECK_EQ(rss_gpio_config_check("49"), -1);
	CHECK_EQ(rec.calls, (size_t)1);
	CHECK_EQ(rec.priority[0], LOG_ERR);
	CHECK(strstr(rec.text[0], "[-] Malformed GPIO config") == rec.text[0]);
	CHECK(strstr(rec.text[0], "'49'") != NULL);

	restore_default();
	CHECK_EQ(rss_gpio_config_check("49"), -1);
	CHECK_EQ(rec.calls, (size_t)1);
	CHECK_EQ(captured.calls, (size_t)1);
	CHECK_EQ(strcmp(captured.text[0], rec.text[0]), 0);

	restore_default();
}

/*
 * A message longer than the buffer is cut to 255 bytes and ends in a
 * NUL; nothing is written past the buffer.
 */
static void test_long_message_is_truncated(void)
{
	static struct record rec;
	static char big[1001];

	memset(big, 'a', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';

	reset(&rec);
	rss_set_log_handler(recording_handler, &rec);

	CHECK_EQ(rss_gpio_config_check(big), -1);
	CHECK_EQ(rec.calls, (size_t)1);
	CHECK_EQ(rec.len[0], (size_t)255);
	CHECK_EQ(strlen(rec.text[0]), (size_t)255);
	CHECK(strstr(rec.text[0], "[-] Malformed GPIO config") == rec.text[0]);

	restore_default();
}

struct pair_check {
	pthread_mutex_t lock;
	size_t calls;
	size_t torn;
};

static int ctx_a, ctx_b;

static void count_pair(struct pair_check *pc, int ok)
{
	pthread_mutex_lock(&pc->lock);
	pc->calls++;
	if (!ok)
		pc->torn++;
	pthread_mutex_unlock(&pc->lock);
}

static struct pair_check pair = { PTHREAD_MUTEX_INITIALIZER, 0, 0 };

static void handler_a(int priority, const char *msg, void *ctx)
{
	(void)priority;
	(void)msg;
	count_pair(&pair, ctx == &ctx_a);
}

static void handler_b(int priority, const char *msg, void *ctx)
{
	(void)priority;
	(void)msg;
	count_pair(&pair, ctx == &ctx_b);
}

#define PAIR_ROUNDS 20000

static void *flip_handler(void *arg)
{
	size_t i;

	(void)arg;
	for (i = 0; i < PAIR_ROUNDS; i++) {
		if (i % 2)
			rss_set_log_handler(handler_a, &ctx_a);
		else
			rss_set_log_handler(handler_b, &ctx_b);
	}
	return NULL;
}

static void *log_messages(void *arg)
{
	size_t i;

	(void)arg;
	for (i = 0; i < PAIR_ROUNDS; i++)
		board_log(LOG_ERR, "message %zu", i);
	return NULL;
}

/*
 * While one thread keeps replacing the handler and its context, a
 * logging thread never gets a handler together with the context of
 * the other one.
 */
static void test_pair_is_not_torn(void)
{
	pthread_t setter, logger;

	rss_set_log_handler(handler_a, &ctx_a);
	CHECK_EQ(pthread_create(&setter, NULL, flip_handler, NULL), 0);
	CHECK_EQ(pthread_create(&logger, NULL, log_messages, NULL), 0);
	CHECK_EQ(pthread_join(setter, NULL), 0);
	CHECK_EQ(pthread_join(logger, NULL), 0);

	CHECK_EQ(pair.calls, (size_t)PAIR_ROUNDS);
	CHECK_EQ(pair.torn, (size_t)0);

	restore_default();
}

int main(void)
{
	test_default_goes_to_syslog();
	test_handler_gets_what_syslog_got();
	test_context_is_passed_through();
	test_null_restores_default();
	test_board_message_reaches_handler();
	test_long_message_is_truncated();
	test_pair_is_not_torn();

	return MT_REPORT();
}
