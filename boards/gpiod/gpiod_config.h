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

#ifndef GPIOD_CONFIG_H
#define GPIOD_CONFIG_H

#include <stddef.h>
#include <stdint.h>

/* Size of a buffer for a chip name, including its terminating NUL. */
#define MAX_CHIPNAME_LEN 64

/*
 * config format: "<chip>:<line>", e.g. "gpiochip0:17". A non-NULL
 * config whose chip or line part cannot be parsed is refused, so a
 * typo or a stale config, such as the bare GPIO number of the former
 * BeagleBone Black board, never drives a line the operator did not
 * name. The chip part is a bare chip name such as "gpiochip0", and a
 * name with a "/" is malformed, since it would name a path outside
 * /dev. The line part is accepted only as a non-empty string of the
 * digits 0-9, with no sign, no surrounding whitespace and no trailing
 * garbage -- anything strtoul() would otherwise parse leniently is
 * rejected instead of silently misread. The format is the same for
 * both libgpiod API versions.
 *
 * gpiod_config_parse() applies the format. config must not be NULL;
 * the caller maps NULL to its own default. chipname_out receives the
 * chip name, which must fit chipname_out_len bytes with its NUL, and
 * line_out the line number. Both are written only on success.
 * Returns 0 for a well-formed config and -1 for a malformed one, after
 * logging it once as "[-] Malformed GPIO config" with the reason.
 *
 * The parser needs neither libgpiod nor a board macro, so the x86_64
 * stub applies the same rules as the boards that drive a real line.
 */
int32_t gpiod_config_parse(const char *config, char *chipname_out,
			   size_t chipname_out_len, unsigned int *line_out);

#endif /* GPIOD_CONFIG_H */
