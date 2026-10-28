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
 * Tests rss_gpio_config(): the library keeps its own bounded copy of
 * the string, refuses one that is too long or malformed without
 * disturbing the config in effect, and reports the result through its
 * return value and rss_last_error(). Built against the stub board, so
 * this runs on any host. The stub fails a send with a chosen cause when
 * the chip of the config is "fail" plus a cause word, which is how the
 * tests see which config a send used.
 */
#include <stdint.h>
#include <string.h>

#include "minitest.h"
#include "../librsswitch.h"
#include "../boards/stub/stub.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

/* The library keeps at most 79 characters and a NUL. */
#define BUF_LEN 80

/* Sends once, as the tests below do to see which config is in effect. */
static int32_t send_once(void)
{
	stub_recorder_reset();
	return rss_send(RSS_POLLIN_2605, 17, 23, 1);
}

/*
 * Fills buf with a chip name of chiplen characters, a colon and
 * ndigits (at least two) digits that read 17, and returns buf.
 */
static char *make_config(char *buf, size_t chiplen, size_t ndigits)
{
	size_t i;

	memset(buf, 'a', chiplen);
	buf[chiplen] = ':';
	for (i = 0; i < ndigits; i++)
		buf[chiplen + 1 + i] = '0';
	buf[chiplen + ndigits] = '7';
	buf[chiplen + ndigits - 1] = '1';
	buf[chiplen + 1 + ndigits] = '\0';
	return buf;
}

/*
 * The caller's string is copied: once the call has returned, changing
 * or wiping the caller's buffer does not change what the next send
 * uses.
 */
static void test_string_is_copied(void)
{
	char buf[BUF_LEN];

	strcpy(buf, "failbusy:0");
	CHECK_EQ(rss_gpio_config(buf), 0);
	CHECK_EQ(rss_last_error(), RSS_ERR_NONE);

	strcpy(buf, "gpiochip0:17");
	CHECK_EQ(send_once(), -1);
	CHECK_EQ(rss_last_error(), RSS_ERR_BUSY);

	memset(buf, 'x', sizeof(buf));
	CHECK_EQ(send_once(), -1);
	CHECK_EQ(rss_last_error(), RSS_ERR_BUSY);
	CHECK_EQ(rss_send_codeword("0FFF0000F00F", 350), -1);
	CHECK_EQ(rss_last_error(), RSS_ERR_BUSY);

	CHECK_EQ(rss_gpio_config(NULL), 0);
	CHECK_EQ(send_once(), 0);
	CHECK_EQ(rss_last_error(), RSS_ERR_NONE);
}

/* NULL restores the board default and succeeds. */
static void test_null_restores_default(void)
{
	CHECK_EQ(rss_gpio_config("failio:0"), 0);
	CHECK_EQ(send_once(), -1);
	CHECK_EQ(rss_last_error(), RSS_ERR_IO);

	CHECK_EQ(rss_gpio_config(NULL), 0);
	CHECK_EQ(rss_last_error(), RSS_ERR_NONE);
	CHECK_EQ(send_once(), 0);
	CHECK_EQ(rss_last_error(), RSS_ERR_NONE);
}

/*
 * The longest chip name the grammar allows is 63 characters and the
 * largest line number has 10 digits; the buffer takes 79 characters.
 */
static void test_longest_values_are_accepted(void)
{
	char buf[256];

	CHECK_EQ(rss_gpio_config(make_config(buf, 63, 2)), 0);
	CHECK_EQ(strlen(buf), (size_t)66);
	CHECK_EQ(send_once(), 0);
	CHECK_EQ(rss_last_error(), RSS_ERR_NONE);

	strcpy(buf + 63, ":4294967295");
	CHECK_EQ(strlen(buf), (size_t)74);
	CHECK_EQ(rss_gpio_config(buf), 0);
	CHECK_EQ(send_once(), 0);

	CHECK_EQ(rss_gpio_config(make_config(buf, 63, 15)), 0);
	CHECK_EQ(strlen(buf), (size_t)79);
	CHECK_EQ(send_once(), 0);
	CHECK_EQ(rss_last_error(), RSS_ERR_NONE);

	CHECK_EQ(rss_gpio_config(NULL), 0);
}

/*
 * A string of 80 or more characters is refused, however well formed,
 * and so is one that is not terminated within the buffer size: the
 * library never scans a caller's buffer further than that.
 */
static void test_too_long_is_refused(void)
{
	static char big[4096];
	char buf[256];
	char raw[BUF_LEN];

	CHECK_EQ(rss_gpio_config("failnochip:0"), 0);

	CHECK_EQ(rss_gpio_config(make_config(buf, 63, 16)), -1);
	CHECK_EQ(strlen(buf), (size_t)80);
	CHECK_EQ(rss_last_error(), RSS_ERR_ARG);
	CHECK_EQ(send_once(), -1);
	CHECK_EQ(rss_last_error(), RSS_ERR_NOCHIP);

	memset(big, 'a', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	CHECK_EQ(rss_gpio_config(big), -1);
	CHECK_EQ(rss_last_error(), RSS_ERR_ARG);
	big[10] = ':';
	CHECK_EQ(rss_gpio_config(big), -1);
	CHECK_EQ(rss_last_error(), RSS_ERR_ARG);

	/* 80 characters and no NUL: the scan has to stop at the bound. */
	memset(raw, 'a', sizeof(raw));
	CHECK_EQ(rss_gpio_config(raw), -1);
	CHECK_EQ(rss_last_error(), RSS_ERR_ARG);

	CHECK_EQ(send_once(), -1);
	CHECK_EQ(rss_last_error(), RSS_ERR_NOCHIP);

	CHECK_EQ(rss_gpio_config(NULL), 0);
}

/*
 * A string that fails the chip:line grammar is refused with
 * RSS_ERR_ARG, and the config in effect stays.
 */
static void test_malformed_is_refused(void)
{
	static const char * const malformed[] = {
		"49", "failbusy", "gpiochip0", ":17", "gpiochip0:",
		"gpiochip0:17x", "/dev/gpiochip0:17", "..:3", "",
	};
	size_t i;

	CHECK_EQ(rss_gpio_config("failnochip:0"), 0);

	for (i = 0; i < ARRAY_SIZE(malformed); i++) {
		CHECK_EQ(rss_gpio_config(malformed[i]), -1);
		CHECK_EQ(rss_last_error(), RSS_ERR_ARG);
		CHECK_EQ(send_once(), -1);
		CHECK_EQ(rss_last_error(), RSS_ERR_NOCHIP);
	}

	CHECK_EQ(rss_gpio_config(NULL), 0);
}

/*
 * A refused string sets the cause, and the next accepted one clears
 * it. rss_gpio_config_check() changes the cause in neither case.
 */
static void test_last_error_follows_the_call(void)
{
	CHECK_EQ(rss_gpio_config("49"), -1);
	CHECK_EQ(rss_last_error(), RSS_ERR_ARG);

	CHECK_EQ(rss_gpio_config_check("gpiochip0:17"), 0);
	CHECK_EQ(rss_last_error(), RSS_ERR_ARG);

	CHECK_EQ(rss_gpio_config("gpiochip0:17"), 0);
	CHECK_EQ(rss_last_error(), RSS_ERR_NONE);

	CHECK_EQ(rss_gpio_config_check("49"), -1);
	CHECK_EQ(rss_last_error(), RSS_ERR_NONE);

	CHECK_EQ(rss_gpio_config(NULL), 0);
}

int main(void)
{
	test_string_is_copied();
	test_null_restores_default();
	test_longest_values_are_accepted();
	test_too_long_is_refused();
	test_malformed_is_refused();
	test_last_error_follows_the_call();

	return MT_REPORT();
}
