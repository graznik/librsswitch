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

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "gpiod_config.h"

/*
 * Tests the character as unsigned char against the three ASCII ranges
 * instead of calling isalnum(), whose answer depends on the locale.
 */
static int is_alnum_ascii(char c)
{
	unsigned char u = (unsigned char)c;

	return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') ||
	       (u >= 'A' && u <= 'Z');
}

int32_t gpiod_config_parse(const char *config, char *chipname_out,
			   size_t chipname_out_len, unsigned int *line_out)
{
	const char *sep;
	const char *linestr;
	size_t chiplen;
	size_t i;
	unsigned long parsed_line;
	char *end;

	sep = strchr(config, ':');
	if (!sep) {
		syslog(LOG_ERR,
		       "[-] Malformed GPIO config '%s' (expected chip:line)",
		       config);
		return -1;
	}

	chiplen = (size_t)(sep - config);
	if (chiplen == 0 || chiplen >= chipname_out_len ||
	    !is_alnum_ascii(config[0]) ||
	    memchr(config, '/', chiplen) != NULL) {
		syslog(LOG_ERR,
		       "[-] Malformed GPIO config '%s' (bad chip name)",
		       config);
		return -1;
	}

	linestr = sep + 1;
	if (linestr[0] == '\0')
		goto bad_line;
	for (i = 0; linestr[i] != '\0'; i++) {
		if (linestr[i] < '0' || linestr[i] > '9')
			goto bad_line;
	}

	errno = 0;
	parsed_line = strtoul(linestr, &end, 10);
	if (errno == ERANGE)
		goto bad_line;
	if (parsed_line > UINT_MAX)
		goto bad_line;
	if (*end != '\0')
		goto bad_line;

	memcpy(chipname_out, config, chiplen);
	chipname_out[chiplen] = '\0';
	*line_out = (unsigned int)parsed_line;
	return 0;

bad_line:
	syslog(LOG_ERR,
	       "[-] Malformed GPIO config '%s' (bad line number)",
	       config);
	return -1;
}
