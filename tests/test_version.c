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
 * Tests the version API: the three numeric macros, the string built
 * from them, and rss_version(), which reports the version compiled into
 * the library. Built against the stub board, so this runs on any host.
 */
#include <stdio.h>
#include <string.h>

#include "minitest.h"
#include "../librsswitch.h"

static void test_version_is_not_null(void)
{
	CHECK(rss_version() != NULL);
}

static void test_version_matches_header(void)
{
	CHECK(strcmp(rss_version(), LIBRSSWITCH_VERSION) == 0);
}

/* The string is built from the numbers, so it cannot drift from them. */
static void test_string_is_built_from_numbers(void)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "%d.%d.%d", LIBRSSWITCH_VERSION_MAJOR,
		 LIBRSSWITCH_VERSION_MINOR, LIBRSSWITCH_VERSION_PATCH);
	CHECK(strcmp(buf, LIBRSSWITCH_VERSION) == 0);
	CHECK(strcmp(buf, rss_version()) == 0);
}

/* The major version is the SONAME major of the shared library. */
static void test_major_is_one(void)
{
	CHECK_EQ(LIBRSSWITCH_VERSION_MAJOR, 1);
}

static void test_version_is_static_storage(void)
{
	CHECK(rss_version() == rss_version());
}

int main(void)
{
	test_version_is_not_null();
	test_version_matches_header();
	test_string_is_built_from_numbers();
	test_major_is_one();
	test_version_is_static_storage();

	return MT_REPORT();
}
