#!/bin/sh
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

# Builds the shared and the static library of each board and checks
# them: the file names and links, the SONAME, the members of the
# archive, that the shared library exports exactly the functions
# marked RSS_API in librsswitch.h, all at the symbol version of the
# SONAME, and nothing of the board contract, and that neither the
# library nor a program linked against it carries an RPATH or RUNPATH.
# It then builds a small program against the stub library, runs it
# through the dynamic loader and checks its output. Takes the root of
# the code repository as $1; run by `make test`. The libgpiod board is
# skipped with one line when libgpiod cannot be linked.
#
# The libraries are built in a copy of the sources below the temporary
# directory, so the checkout is not written to and may be on a file
# system that cannot make symbolic links.

if [ -z "$1" ] || [ ! -f "$1/tests/Makefile" ]; then
	echo "usage: $0 <code repository root>" >&2
	exit 2
fi
ROOT=$(cd "$1" && pwd -P)

total=0
passed=0

# check DESC COMMAND...
check()
{
	desc=$1
	shift

	total=$((total + 1))
	if "$@"; then
		passed=$((passed + 1))
	else
		echo "FAIL $desc" >&2
	fi
}

S=$(mktemp -d) || exit 1
trap 'rm -rf "$S"' EXIT
trap 'exit 1' HUP INT TERM

TREE="$S/tree"
mkdir "$TREE" || exit 1
cp -R "$ROOT/Makefile" "$ROOT/librsswitch.c" "$ROOT/librsswitch.h" \
	"$ROOT/librsswitch.map" "$ROOT/boards" "$TREE" || exit 1

# The version, read from the header the way the Makefile reads it.
header_number()
{
	awk -v n="LIBRSSWITCH_VERSION_$1" '$2 == n { print $3 }' \
		"$ROOT/librsswitch.h"
}
MAJOR=$(header_number MAJOR)
MINOR=$(header_number MINOR)
PATCH=$(header_number PATCH)
VERSION="$MAJOR.$MINOR.$PATCH"

# have_gpiod: a program that includes <gpiod.h> links with -lgpiod.
have_gpiod()
{
	printf '%s\n' '#include <gpiod.h>' \
		'int main(void) { return 0; }' > "$S/probe.c" &&
		cc $CPPFLAGS "$S/probe.c" $LDFLAGS -lgpiod \
			-o "$S/probe" > /dev/null 2>&1
}

BOARDS=stub
if have_gpiod; then
	BOARDS="gpiod $BOARDS"
else
	echo "SKIP gpiod board: gpiod.h or libgpiod not found" >&2
fi

# build_board BOARD: builds the libraries of BOARD in the copy. The
# knobs are emptied, so a DEBUG or CROSS_COMPILE in the environment of
# the caller does not change what is checked.
build_board()
{
	(
		unset MAKEFLAGS MFLAGS MAKELEVEL
		make -s -C "$TREE" BOARD="$1" DEBUG= LIBGPIOD_VERSION= \
			CROSS_COMPILE=
	) > "$S/make-$1.log" 2>&1 || {
		sed 's/^/  /' "$S/make-$1.log" >&2
		return 1
	}
}

for b in $BOARDS; do
	check "BOARD=$b builds the libraries" build_board "$b"
done

# The functions the header marks as public, one name per line.
want=$(sed -n -E 's/^RSS_API[^(]*[ *]([A-Za-z_0-9]+)\(.*/\1/p' \
	"$ROOT/librsswitch.h" | LC_ALL=C sort)

# exports FILE: the names of the symbols FILE defines for other objects
# to use, without a version suffix, one per line.
exports()
{
	nm -D --defined-only "$1" | awk '$2 ~ /^[TDBRVW]$/ { print $3 }' |
		sed 's/@.*//' | LC_ALL=C sort
}

# An empty list would link, and every program would then fail to start,
# so the list of the header must not be empty.
want_listed()
{
	[ -n "$want" ] && ! printf '%s\n' "$want" | grep -qv '^rss_'
}
check "librsswitch.h marks its functions with RSS_API" want_listed

# so_checks BOARD: the shared library of BOARD.
so_checks()
{
	dir="$TREE/build/$1"
	so="$dir/librsswitch.so.$VERSION"

	[ -f "$so" ] || return 1
	readelf -d "$so" |
		grep -q "SONAME.*\[librsswitch\.so\.$MAJOR\]" || return 1
	[ "$(readlink "$dir/librsswitch.so.$MAJOR")" = \
		"librsswitch.so.$VERSION" ] || return 1
	[ "$(readlink "$dir/librsswitch.so")" = \
		"librsswitch.so.$MAJOR" ] || return 1
}

# exports_are_api BOARD: the shared library exports exactly the
# functions of the header.
exports_are_api()
{
	so="$TREE/build/$1/librsswitch.so.$VERSION"

	[ -f "$so" ] && [ -n "$want" ] &&
		[ "$(exports "$so")" = "$want" ]
}

# no_contract_names BOARD: no name of the board contract, the libgpiod
# helpers or the stub's test hooks is exported.
no_contract_names()
{
	so="$TREE/build/$1/librsswitch.so.$VERSION"

	[ -f "$so" ] || return 1
	! nm -D --defined-only "$so" |
		grep -qE 'board_|transmit|gpiod_|delayMicro|stub_'
}

# version_node BOARD: the version node is named after the major version,
# and every exported function is bound to it.
version_node()
{
	so="$TREE/build/$1/librsswitch.so.$VERSION"

	[ -f "$so" ] || return 1
	readelf -V "$so" | grep -q "Name: LIBRSSWITCH_$MAJOR" || return 1
	! nm -D --defined-only "$so" |
		awk -v v="@@LIBRSSWITCH_$MAJOR" '
			$2 ~ /^[TDBRVW]$/ && substr($3, length($3) - length(v) + 1) != v {
				bad = 1
			}
			END { exit !bad }'
}

# needed_gpiod BOARD: the libgpiod board needs libgpiod, the stub board
# does not.
needed_gpiod()
{
	so="$TREE/build/$1/librsswitch.so.$VERSION"

	[ -f "$so" ] || return 1
	if [ "$1" = gpiod ]; then
		readelf -d "$so" | grep -q 'NEEDED.*libgpiod'
	else
		! readelf -d "$so" | grep -q 'NEEDED.*libgpiod'
	fi
}

# no_rpath FILE: FILE has no RPATH or RUNPATH entry.
no_rpath()
{
	[ -f "$1" ] && ! readelf -d "$1" | grep -Eq 'RPATH|RUNPATH'
}

# has_debug_info BOARD: the shared library carries debug information.
has_debug_info()
{
	readelf -S "$TREE/build/$1/librsswitch.so.$VERSION" |
		grep -q '\.debug_info'
}

# members_are BOARD: the archive holds the objects of the core and of
# the board, and nothing else.
members_are()
{
	if [ "$1" = gpiod ]; then
		board_obj=gpiod_board.o
	else
		board_obj=stub.o
	fi
	got=$(ar t "$TREE/build/$1/librsswitch.a" | LC_ALL=C sort) || return 1
	[ "$got" = "$(printf '%s\n' gpiod_config.o librsswitch.o \
		"$board_obj" | LC_ALL=C sort)" ]
}

# The functions that the internal headers declare: the board contract,
# the config parser and the stub's test hooks. They stay global in the
# archive, which links the tests, and are not exported by the shared
# library. A declaration starts in the first column.
declared_internal()
{
	for h in boards/board.h boards/gpiod/gpiod_config.h \
		boards/stub/stub.h; do
		sed -n -E 's/^[a-z][^(;]*[ *]([a-z_0-9]+)\(.*/\1/p' \
			"$ROOT/$h"
	done
}

# archive_globals_known BOARD: the archive defines no global name other
# than the public functions and the internal ones that the headers
# declare, so no stray name reaches the program that links it.
archive_globals_known()
{
	ar="$TREE/build/$1/librsswitch.a"

	[ -f "$ar" ] || return 1
	nm -g --defined-only "$ar" |
		awk '$2 ~ /^[TDBRVWC]$/ { print $3 }' | LC_ALL=C sort -u \
		> "$S/archive-$1.names"
	{ printf '%s\n' "$want"; declared_internal; } | LC_ALL=C sort -u \
		> "$S/allowed.names"
	extra=$(LC_ALL=C comm -23 "$S/archive-$1.names" "$S/allowed.names")
	[ -z "$extra" ] || printf '  global in the archive: %s\n' \
		"$extra" >&2
	[ -z "$extra" ] && [ -s "$S/archive-$1.names" ]
}

for b in $BOARDS; do
	check "$b: file names, SONAME and links" so_checks "$b"
	check "$b: the shared library exports exactly the public functions" \
		exports_are_api "$b"
	check "$b: no board contract or test hook name is exported" \
		no_contract_names "$b"
	check "$b: the exports are bound to the version node" \
		version_node "$b"
	check "$b: the shared library needs libgpiod only for gpiod" \
		needed_gpiod "$b"
	check "$b: the shared library has no RPATH or RUNPATH" \
		no_rpath "$TREE/build/$b/librsswitch.so.$VERSION"
	check "$b: the shared library has debug information" \
		has_debug_info "$b"
	check "$b: the archive holds the core and the board" members_are "$b"
	check "$b: the archive defines only known global names" \
		archive_globals_known "$b"
done

# Both boards build side by side: the one built second does not remove
# the first.
side_by_side()
{
	for b in $BOARDS; do
		[ -f "$TREE/build/$b/librsswitch.so.$VERSION" ] || return 1
		[ -f "$TREE/build/$b/librsswitch.a" ] || return 1
	done
}
check "the boards build side by side" side_by_side

# A board that is not known stops make with a message.
bad_board()
{
	(
		unset MAKEFLAGS MFLAGS MAKELEVEL
		make -s -C "$TREE" BOARD=bogus
	) > "$S/bogus.log" 2>&1 && return 1
	grep -q 'BOARD must be gpiod or stub' "$S/bogus.log"
}
check "an unknown BOARD stops make" bad_board

# A program that links the stub library and runs it through the dynamic
# loader: it sends Pollin 17/23 On, which the stub prints, and then
# prints the version of the library it loaded. The loader finds the
# library through LD_LIBRARY_PATH only.
printf '%s\n' '#include <stdio.h>' \
	'#include "librsswitch.h"' \
	'' \
	'int main(void)' \
	'{' \
	'	if (rss_send(RSS_POLLIN_2605, 17, 23, 1) != 0)' \
	'		return 1;' \
	'	puts(rss_version());' \
	'	return 0;' \
	'}' > "$S/consumer.c"

consumer_runs()
{
	cc -Wall -Werror -I"$TREE" -o "$S/consumer" "$S/consumer.c" \
		-L"$TREE/build/stub" -lrsswitch || return 1
	LD_LIBRARY_PATH="$TREE/build/stub" "$S/consumer" > "$S/consumer.out" ||
		return 1
	printf '%s\n%s\n' 0FFF0000F00F "$VERSION" > "$S/consumer.want"
	cmp -s "$S/consumer.out" "$S/consumer.want"
}
check "a program built against the stub library runs and prints" \
	consumer_runs

consumer_needed()
{
	[ -x "$S/consumer" ] || return 1
	readelf -d "$S/consumer" |
		grep -q "NEEDED.*\[librsswitch\.so\.$MAJOR\]"
}
check "the program needs the SONAME of the library" consumer_needed
check "the program has no RPATH or RUNPATH" no_rpath "$S/consumer"

echo "$passed/$total checks passed" >&2
[ "$passed" -eq "$total" ] || exit 1
