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
 * Minimal single-header test harness. No external dependency beyond
 * libc, on purpose -- this project has none today (only librt/libgpiod
 * on real boards), and the x86_64 stub target already exists to make
 * the protocol logic host-testable without hardware.
 */
#ifndef MINITEST_H
#define MINITEST_H

#include <stdio.h>

static int mt_run;
static int mt_fail;

#define CHECK(cond) do { \
	mt_run++; \
	if (!(cond)) { \
		mt_fail++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

#define MT_REPORT() \
	(fprintf(stderr, "%d/%d checks passed\n", mt_run - mt_fail, mt_run), \
	 mt_fail == 0 ? 0 : 1)

#endif /* MINITEST_H */
