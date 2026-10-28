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
 * Tests rss_send()'s return-code contract for both supported
 * devices. Built against the stub board, so this runs on any
 * host, no hardware needed. Codeword content is checked in
 * test_codewords.c; this file covers the return-code and
 * no-transmission contract.
 */
#include "minitest.h"
#include "../librsswitch.h"
#include "../boards/board.h"
#include "../boards/stub/stub.h"
#include "pulse_decode.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

static void test_rev_008345_valid(void)
{
	uint32_t group, socket, data;

	for (group = 0; group < 4; group++)
		for (socket = 0; socket < 3; socket++)
			for (data = 0; data < 2; data++)
				CHECK_EQ(rss_send(RSS_REV_008345, group, socket, data), 0);
}

static void test_rev_008345_out_of_range(void)
{
	CHECK_EQ(rss_send(RSS_REV_008345, 4, 0, 0), -1);   /* group */
	CHECK_EQ(rss_send(RSS_REV_008345, 0, 3, 0), -1);   /* socket */
	CHECK_EQ(rss_send(RSS_REV_008345, 0, 0, 2), -1);   /* data */
}

static void test_pollin_2605_valid(void)
{
	CHECK_EQ(rss_send(RSS_POLLIN_2605, 0, 0, 0), 0);
	CHECK_EQ(rss_send(RSS_POLLIN_2605, 31, 31, 1), 0);
	CHECK_EQ(rss_send(RSS_POLLIN_2605, 15, 15, 0), 0);
}

static void test_pollin_2605_out_of_range(void)
{
	CHECK_EQ(rss_send(RSS_POLLIN_2605, 32, 0, 0), -1);  /* group */
	CHECK_EQ(rss_send(RSS_POLLIN_2605, 0, 32, 0), -1);  /* socket */
	CHECK_EQ(rss_send(RSS_POLLIN_2605, 0, 0, 2), -1);   /* data */
}

static void test_unknown_device(void)
{
	CHECK_EQ(rss_send(99, 0, 0, 0), -1);
}

static void test_rev_008345_boundary(void)
{
	stub_recorder_reset();
	CHECK_EQ(rss_send(RSS_REV_008345, 3, 2, 0), 0);
	CHECK_EQ(expect_transmission("FFF1FF100001", 350), 0);
}

static void test_pollin_2605_boundary(void)
{
	stub_recorder_reset();
	CHECK_EQ(rss_send(RSS_POLLIN_2605, 31, 31, 0), 0);
	CHECK_EQ(expect_transmission("0000000000F0", 350), 0);
}

/*
 * A rejected address must not leave a partial transmission on the
 * air, and must not open a board session at all: every out-of-range
 * call the tests above already reject must record zero pulses and
 * make zero board_init()/board_exit() calls. The final control send
 * shows exactly one of each, so the counters are not vacuous.
 */
static void test_rejected_sends_transmit_nothing(void)
{
	stub_recorder_reset();
	CHECK_EQ(rss_send(RSS_REV_008345, 4, 0, 0), -1);   /* group */
	CHECK_EQ(stub_recorder_count(), (size_t)0);
	CHECK_EQ(stub_board_init_count(), (size_t)0);
	CHECK_EQ(stub_board_exit_count(), (size_t)0);

	stub_recorder_reset();
	CHECK_EQ(rss_send(RSS_REV_008345, 0, 3, 0), -1);   /* socket */
	CHECK_EQ(stub_recorder_count(), (size_t)0);
	CHECK_EQ(stub_board_init_count(), (size_t)0);
	CHECK_EQ(stub_board_exit_count(), (size_t)0);

	stub_recorder_reset();
	CHECK_EQ(rss_send(RSS_REV_008345, 0, 0, 2), -1);   /* data */
	CHECK_EQ(stub_recorder_count(), (size_t)0);
	CHECK_EQ(stub_board_init_count(), (size_t)0);
	CHECK_EQ(stub_board_exit_count(), (size_t)0);

	stub_recorder_reset();
	CHECK_EQ(rss_send(RSS_POLLIN_2605, 32, 0, 0), -1);  /* group */
	CHECK_EQ(stub_recorder_count(), (size_t)0);
	CHECK_EQ(stub_board_init_count(), (size_t)0);
	CHECK_EQ(stub_board_exit_count(), (size_t)0);

	stub_recorder_reset();
	CHECK_EQ(rss_send(RSS_POLLIN_2605, 0, 32, 0), -1);  /* socket */
	CHECK_EQ(stub_recorder_count(), (size_t)0);
	CHECK_EQ(stub_board_init_count(), (size_t)0);
	CHECK_EQ(stub_board_exit_count(), (size_t)0);

	stub_recorder_reset();
	CHECK_EQ(rss_send(RSS_POLLIN_2605, 0, 0, 2), -1);   /* data */
	CHECK_EQ(stub_recorder_count(), (size_t)0);
	CHECK_EQ(stub_board_init_count(), (size_t)0);
	CHECK_EQ(stub_board_exit_count(), (size_t)0);

	stub_recorder_reset();
	CHECK_EQ(rss_send(99, 0, 0, 0), -1);            /* device */
	CHECK_EQ(stub_recorder_count(), (size_t)0);
	CHECK_EQ(stub_board_init_count(), (size_t)0);
	CHECK_EQ(stub_board_exit_count(), (size_t)0);

	/* Control: a valid send still opens exactly one board session. */
	stub_recorder_reset();
	CHECK_EQ(rss_send(RSS_POLLIN_2605, 0, 0, 0), 0);
	CHECK_EQ(stub_board_init_count(), (size_t)1);
	CHECK_EQ(stub_board_exit_count(), (size_t)1);
}

/*
 * A failing transmit() must abort the rest of the codeword: no pulse
 * after the failing one is recorded, board_exit() still runs exactly
 * once, and rss_send() returns -1. Exercised at four points across
 * a 12-symbol, 10-repetition RSS_POLLIN_2605 codeword: the very first
 * pulse, inside a symbol, at the first sync pulse, and inside the
 * second frame.
 */
static void test_transmit_failure_aborts_codeword(void)
{
	static const size_t fail_points[] = {0, 5, 24, 30};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(fail_points); i++) {
		size_t idx = fail_points[i];

		stub_recorder_reset();
		stub_recorder_fail_at(idx);
		CHECK_EQ(rss_send(RSS_POLLIN_2605, 17, 23, 1), -1);
		CHECK_EQ(stub_recorder_count(), idx);
		CHECK_EQ(stub_board_exit_count(), (size_t)1);
	}

	stub_recorder_reset();
	CHECK_EQ(rss_send(RSS_POLLIN_2605, 17, 23, 1), 0);
	CHECK_EQ(expect_transmission("0FFF0000F00F", 350), 0);
}

/*
 * rss_addr_limits() reports the same bounds address_check()
 * enforces, for both devices; rejects an unknown dev or any NULL
 * pointer without touching its outputs; and every address at or one
 * past each device's reported limit gets the same accept/reject
 * verdict from rss_send() -- with no board session opened for a
 * rejected address.
 */
static void test_addr_limits(void)
{
	uint32_t ngroups, nsockets, ndata;
	uint32_t sg, ss, sd;
	static const struct {
		enum rss_type dev;
		uint32_t ngroups, nsockets, ndata;
	} devs[] = {
		{ RSS_REV_008345, 4, 3, 2 },
		{ RSS_POLLIN_2605, 32, 32, 2 },
	};
	size_t i;

	CHECK_EQ(rss_addr_limits(RSS_REV_008345, &ngroups, &nsockets, &ndata),
		 0);
	CHECK_EQ(ngroups, (uint32_t)4);
	CHECK_EQ(nsockets, (uint32_t)3);
	CHECK_EQ(ndata, (uint32_t)2);

	CHECK_EQ(rss_addr_limits(RSS_POLLIN_2605, &ngroups, &nsockets, &ndata),
		 0);
	CHECK_EQ(ngroups, (uint32_t)32);
	CHECK_EQ(nsockets, (uint32_t)32);
	CHECK_EQ(ndata, (uint32_t)2);

	sg = ss = sd = 999;
	CHECK_EQ(rss_addr_limits(2, &sg, &ss, &sd), -1);
	CHECK_EQ(sg, (uint32_t)999);
	CHECK_EQ(ss, (uint32_t)999);
	CHECK_EQ(sd, (uint32_t)999);

	CHECK_EQ(rss_addr_limits(RSS_REV_008345, NULL, &nsockets, &ndata), -1);
	CHECK_EQ(rss_addr_limits(RSS_REV_008345, &ngroups, NULL, &ndata), -1);
	CHECK_EQ(rss_addr_limits(RSS_REV_008345, &ngroups, &nsockets, NULL),
		 -1);

	for (i = 0; i < ARRAY_SIZE(devs); i++) {
		stub_recorder_reset();
		CHECK_EQ(rss_send(devs[i].dev, devs[i].ngroups - 1,
				      devs[i].nsockets - 1,
				      devs[i].ndata - 1), 0);
		CHECK_EQ(stub_board_init_count(), (size_t)1);

		stub_recorder_reset();
		CHECK_EQ(rss_send(devs[i].dev, devs[i].ngroups,
				      devs[i].nsockets - 1,
				      devs[i].ndata - 1), -1);
		CHECK_EQ(stub_board_init_count(), (size_t)0);

		stub_recorder_reset();
		CHECK_EQ(rss_send(devs[i].dev, devs[i].ngroups - 1,
				      devs[i].nsockets,
				      devs[i].ndata - 1), -1);
		CHECK_EQ(stub_board_init_count(), (size_t)0);

		stub_recorder_reset();
		CHECK_EQ(rss_send(devs[i].dev, devs[i].ngroups - 1,
				      devs[i].nsockets - 1,
				      devs[i].ndata), -1);
		CHECK_EQ(stub_board_init_count(), (size_t)0);
	}
}

/* GPIO configs the host build must refuse, like the libgpiod boards. */
static const char * const malformed_gpio[] = {
	"49", "17", "gpiochip0", "nocolon", ":17", "gpiochip3:",
	"gpiochip3:17x", "gpiochip3:-1", "gpiochip3:+5", "gpiochip3: 5",
	"gpiochip3:4294967296", "gpiochip0:17:5", "gpiochip0/../x:3",
	"/dev/gpiochip0:17", "../tmp/x:3", "",
	/* A chip name starts with a letter or digit. */
	"..:3", ".:3", ".hidden:3", "-x:3",
};

/*
 * The stub applies the libgpiod boards' "chip:line" rules in
 * board_config_check() and opens no session.
 */
static void test_board_config_check_x86_64(void)
{
	size_t i;

	stub_recorder_reset();

	CHECK_EQ(board_config_check(NULL), 0);
	CHECK_EQ(board_config_check("gpiochip0:17"), 0);
	CHECK_EQ(board_config_check("gpiochip3:5"), 0);
	CHECK_EQ(board_config_check("gpiochip3:007"), 0);

	for (i = 0; i < ARRAY_SIZE(malformed_gpio); i++)
		CHECK_EQ(board_config_check(malformed_gpio[i]), -1);

	CHECK_EQ(stub_board_init_count(), (size_t)0);
}

/*
 * rss_gpio_config_check() answers like the board's check, without
 * storing the string and without opening a session or transmitting.
 */
static void test_gpio_config_check(void)
{
	size_t i;

	stub_recorder_reset();

	CHECK_EQ(rss_gpio_config_check(NULL), 0);
	CHECK_EQ(rss_gpio_config_check("gpiochip0:17"), 0);
	CHECK_EQ(rss_gpio_config_check("gpiochip3:5"), 0);
	CHECK_EQ(rss_gpio_config_check("gpiochip3:007"), 0);

	for (i = 0; i < ARRAY_SIZE(malformed_gpio); i++)
		CHECK_EQ(rss_gpio_config_check(malformed_gpio[i]), -1);

	CHECK_EQ(stub_board_init_count(), (size_t)0);
	CHECK_EQ(stub_recorder_count(), (size_t)0);
}

/*
 * A GPIO config whose chip is "fail" plus a cause word makes the stub
 * fail where hardware would: in board_init() with that cause. Neither
 * send function transmits a pulse or calls board_exit() after it, both
 * report the cause, and the config alone decides, so the next send
 * fails the same way until the config is cleared.
 */
static void test_injected_init_causes(void)
{
	static const struct {
		const char *config;
		enum rss_err cause;
	} causes[] = {
		{ "failnochip:0", RSS_ERR_NOCHIP },
		{ "failbusy:0", RSS_ERR_BUSY },
		{ "faildenied:0", RSS_ERR_DENIED },
		{ "failbadline:0", RSS_ERR_BADLINE },
		{ "failother:0", RSS_ERR_OTHER },
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(causes); i++) {
		stub_recorder_reset();
		rss_gpio_config(causes[i].config);

		CHECK_EQ(rss_send(RSS_POLLIN_2605, 17, 23, 1), -1);
		CHECK_EQ(rss_last_error(), causes[i].cause);
		CHECK_EQ(rss_send_codeword("0FFF0000F00F", 350), -1);
		CHECK_EQ(rss_last_error(), causes[i].cause);

		CHECK_EQ(stub_recorder_count(), (size_t)0);
		CHECK_EQ(stub_board_init_count(), (size_t)2);
		CHECK_EQ(stub_board_exit_count(), (size_t)0);

		rss_gpio_config(NULL);
		stub_recorder_reset();
	}
}

/*
 * failio lets board_init() succeed and fails the first transmit(), so
 * the session is opened and closed like any other and no pulse is
 * recorded.
 */
static void test_injected_io_cause(void)
{
	stub_recorder_reset();
	rss_gpio_config("failio:0");

	CHECK_EQ(rss_send(RSS_POLLIN_2605, 17, 23, 1), -1);
	CHECK_EQ(rss_last_error(), RSS_ERR_IO);
	CHECK_EQ(rss_send_codeword("0FFF0000F00F", 350), -1);
	CHECK_EQ(rss_last_error(), RSS_ERR_IO);

	CHECK_EQ(stub_recorder_count(), (size_t)0);
	CHECK_EQ(stub_board_init_count(), (size_t)2);
	CHECK_EQ(stub_board_exit_count(), (size_t)2);
	CHECK_EQ(stub_board_overlap_count(), (size_t)0);

	rss_gpio_config(NULL);
	stub_recorder_reset();
	CHECK_EQ(rss_send(RSS_POLLIN_2605, 17, 23, 1), 0);
	CHECK_EQ(rss_last_error(), RSS_ERR_NONE);
	CHECK_EQ(expect_transmission("0FFF0000F00F", 350), 0);
}

/*
 * Only the exact chip name before the colon injects a failure: a name
 * that merely resembles one, in any case or with a prefix or suffix,
 * and every real chip send normally.
 */
static void test_injection_needs_exact_name(void)
{
	static const char * const sends[] = {
		"failbusyx:0", "fail:0", "failBUSY:0", "xfailbusy:0",
		"failbusy", "gpiochip0:17",
	};
	static const char * const injecting[] = {
		"failnochip:0", "failbusy:0", "faildenied:0",
		"failbadline:0", "failio:0", "failother:0",
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(sends); i++) {
		stub_recorder_reset();
		rss_gpio_config(sends[i]);

		CHECK_EQ(rss_send(RSS_POLLIN_2605, 17, 23, 1), 0);
		CHECK_EQ(rss_last_error(), RSS_ERR_NONE);
		CHECK_EQ(expect_transmission("0FFF0000F00F", 350), 0);
	}

	for (i = 0; i < ARRAY_SIZE(injecting); i++)
		CHECK_EQ(board_config_check(injecting[i]), 0);

	rss_gpio_config(NULL);
	stub_recorder_reset();
}

/*
 * A failed injected board_init() opens no board session, so a send
 * that follows it is not counted as an overlap. A pending failio state
 * dies with stub_recorder_reset().
 */
static void test_injection_opens_no_session(void)
{
	stub_recorder_reset();
	rss_gpio_config("failbusy:0");
	CHECK_EQ(rss_send(RSS_POLLIN_2605, 17, 23, 1), -1);
	CHECK_EQ(rss_send(RSS_POLLIN_2605, 17, 23, 1), -1);
	rss_gpio_config(NULL);
	CHECK_EQ(rss_send(RSS_POLLIN_2605, 17, 23, 1), 0);
	CHECK_EQ(stub_board_init_count(), (size_t)3);
	CHECK_EQ(stub_board_exit_count(), (size_t)1);
	CHECK_EQ(stub_board_overlap_count(), (size_t)0);

	stub_recorder_reset();
	rss_gpio_config("failio:0");
	CHECK_EQ(rss_send(RSS_POLLIN_2605, 17, 23, 1), -1);
	rss_gpio_config(NULL);
	stub_recorder_reset();
	CHECK_EQ(rss_send(RSS_POLLIN_2605, 17, 23, 1), 0);
	CHECK_EQ(stub_recorder_count() > 0, 1);
	CHECK_EQ(stub_board_overlap_count(), (size_t)0);
}

int main(void)
{
	test_rev_008345_valid();
	test_rev_008345_out_of_range();
	test_rev_008345_boundary();
	test_pollin_2605_valid();
	test_pollin_2605_out_of_range();
	test_pollin_2605_boundary();
	test_unknown_device();
	test_rejected_sends_transmit_nothing();
	test_transmit_failure_aborts_codeword();
	test_addr_limits();
	test_board_config_check_x86_64();
	test_gpio_config_check();
	test_injected_init_causes();
	test_injected_io_cause();
	test_injection_needs_exact_name();
	test_injection_opens_no_session();

	return MT_REPORT();
}
