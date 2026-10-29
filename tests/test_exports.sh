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
# them: the file names and links, the SONAME, that the static library
# librsswitch.a holds the whole library as one object and defines only
# the public functions as global names, so that a program with its own
# transmit() and board_init() links it statically, and that
# librsswitch-internal.a, which the tests link, holds the objects as
# they are and keeps the internal names, that the shared library
# exports exactly the functions
# marked RSS_API in librsswitch.h, all at the symbol version of the
# SONAME, and nothing of the board contract, and that neither the
# library nor a program linked against it carries an RPATH or RUNPATH.
# The two programs of the repository are built against each board and
# must need the SONAME of the library, no libgpiod of their own and
# carry no RPATH or RUNPATH; rsssend is run against the stub library.
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

# The tests build in the default directory of a copy. A BUILD in the
# environment of the caller would move the libraries away from the
# paths they look at.
unset BUILD

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
mkdir -p "$TREE/examples/basic" "$TREE/examples/network" || exit 1
cp "$ROOT/examples/basic/Makefile" "$ROOT/examples/basic/rsssend.c" \
	"$TREE/examples/basic" || exit 1
cp "$ROOT/examples/network/Makefile" "$ROOT/examples/network/rsswitchd.c" \
	"$ROOT/examples/network/rsswitchd_proto.c" \
	"$ROOT/examples/network/rsswitchd_proto.h" \
	"$TREE/examples/network" || exit 1

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

# archive_is_one_object BOARD: the installed archive holds the whole
# library as one object, and nothing else.
archive_is_one_object()
{
	[ "$(ar t "$TREE/build/$1/librsswitch.a")" = librsswitch-static.o ]
}

# globals ARCHIVE: the global names that ARCHIVE defines, without
# duplicates, one per line.
globals()
{
	nm -g --defined-only "$1" |
		awk 'NF == 3 { print $3 }' | LC_ALL=C sort -u
}

# archive_globals_are_api BOARD: the installed archive defines exactly the
# functions of the header as global names, so no other name reaches the
# program that links it.
archive_globals_are_api()
{
	ar="$TREE/build/$1/librsswitch.a"

	[ -f "$ar" ] && [ -n "$want" ] || return 1
	globals "$ar" > "$S/archive-$1.names"
	printf '%s\n' "$want" > "$S/api.names"
	extra=$(LC_ALL=C comm -23 "$S/archive-$1.names" "$S/api.names")
	[ -z "$extra" ] || printf '  global in the archive: %s\n' \
		"$extra" >&2
	[ "$(cat "$S/archive-$1.names")" = "$want" ]
}

# A program with names of its own that the board code of the library
# uses with other signatures. When argc > 1 it sends Pollin 17/23 On
# through the library, which prints the codeword of its own stub board,
# so the program's transmit() must not take the place of that board's.
printf '%s\n' '#include <stdio.h>' \
	'#include "librsswitch.h"' \
	'' \
	'int transmit(int x);' \
	'int board_init(void);' \
	'' \
	'int transmit(int x)' \
	'{' \
	'	return x + 1;' \
	'}' \
	'' \
	'int board_init(void)' \
	'{' \
	'	return 41;' \
	'}' \
	'' \
	'int main(int argc, char **argv)' \
	'{' \
	'	(void)argv;' \
	'	if (transmit(1) != 2 || board_init() != 41)' \
	'		return 1;' \
	'	if (argc > 1 && rss_send(RSS_POLLIN_2605, 17, 23, 1) != 0)' \
	'		return 1;' \
	'	puts(rss_version());' \
	'	return 0;' \
	'}' > "$S/static-consumer.c"

# board_libs DIR: the libraries that the archive in build/DIR needs. A
# DIR is named after its board, or starts with it for a build with
# other flags. The libraries come after the archive on the command
# line, or the linker would not yet see a use of them, and --as-needed
# would drop them.
board_libs()
{
	case $1 in
	gpiod*)
		echo -lgpiod
		;;
	esac
}

# static_consumer_runs DIR: the program links the installed archive of
# build/DIR statically and runs. The stub prints its codeword, which
# shows that the library used its own board; the libgpiod board opens no
# GPIO here.
static_consumer_runs()
{
	libs=$(board_libs "$1")
	# shellcheck disable=SC2086
	cc -Wall -Werror -I"$TREE" -o "$S/static-$1" "$S/static-consumer.c" \
		"$TREE/build/$1/librsswitch.a" $libs -pthread || return 1
	case $1 in
	stub*)
		"$S/static-$1" send > "$S/static-$1.out" || return 1
		printf '%s\n%s\n' 0FFF0000F00F "$VERSION" > "$S/static-$1.want"
		;;
	*)
		"$S/static-$1" > "$S/static-$1.out" || return 1
		printf '%s\n' "$VERSION" > "$S/static-$1.want"
		;;
	esac
	cmp -s "$S/static-$1.out" "$S/static-$1.want" || return 1
	! readelf -d "$S/static-$1" | grep -q 'NEEDED.*librsswitch'
}

# internal_members_are BOARD: the internal archive holds the objects of
# the core and of the board as separate members, and nothing else.
internal_members_are()
{
	if [ "$1" = gpiod ]; then
		board_obj=gpiod_board.o
	else
		board_obj=stub.o
	fi
	got=$(ar t "$TREE/build/$1/librsswitch-internal.a" | LC_ALL=C sort) ||
		return 1
	[ "$got" = "$(printf '%s\n' gpiod_config.o librsswitch.o \
		"$board_obj" | LC_ALL=C sort)" ]
}

# The functions that the internal headers declare: the board contract,
# the config parser and the stub's test hooks. They stay global in the
# internal archive, which links the tests, and are not exported by the
# shared library. A declaration starts in the first column.
declared_internal()
{
	for h in boards/board.h boards/gpiod/gpiod_config.h \
		boards/stub/stub.h; do
		sed -n -E 's/^[a-z][^(;]*[ *]([a-z_0-9]+)\(.*/\1/p' \
			"$ROOT/$h"
	done
}

# internal_globals_known BOARD: the internal archive defines no global
# name other than the public functions and the internal ones that the
# headers declare.
internal_globals_known()
{
	ar="$TREE/build/$1/librsswitch-internal.a"

	[ -f "$ar" ] || return 1
	globals "$ar" > "$S/internal-$1.names"
	{ printf '%s\n' "$want"; declared_internal; } | LC_ALL=C sort -u \
		> "$S/allowed.names"
	extra=$(LC_ALL=C comm -23 "$S/internal-$1.names" "$S/allowed.names")
	[ -z "$extra" ] || printf '  global in the internal archive: %s\n' \
		"$extra" >&2
	[ -z "$extra" ] && [ -s "$S/internal-$1.names" ]
}

# internal_archive_clashes BOARD: linking the same program against the
# internal archive fails with a multiple definition of transmit. The
# tests rely on that archive keeping the internal names global, to reach
# the test hooks and to put a board of their own in front of it, and
# this shows that the checks above can fail.
internal_archive_clashes()
{
	libs=$(board_libs "$1")
	# shellcheck disable=SC2086
	! LC_ALL=C cc -Wall -Werror -I"$TREE" -o "$S/clash-$1" \
		"$S/static-consumer.c" \
		"$TREE/build/$1/librsswitch-internal.a" $libs -pthread \
		> "$S/clash-$1.log" 2>&1 &&
		grep -Eq 'multiple definition of .transmit|duplicate symbol: transmit' \
			"$S/clash-$1.log"
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
	check "$b: the archive holds one object of the whole library" \
		archive_is_one_object "$b"
	check "$b: the archive defines only the public functions as global names" \
		archive_globals_are_api "$b"
	check "$b: a program with its own transmit() and board_init() links the archive" \
		static_consumer_runs "$b"
	check "$b: the internal archive holds the core and the board" \
		internal_members_are "$b"
	check "$b: the internal archive defines only known global names" \
		internal_globals_known "$b"
	check "$b: the internal archive keeps the internal names global" \
		internal_archive_clashes "$b"
done

# build_variant DIR CFLAGS: builds the stub in build/DIR with CFLAGS in
# place of the default flags of the Makefile.
build_variant()
{
	(
		unset MAKEFLAGS MFLAGS MAKELEVEL
		make -s -C "$TREE" BOARD=stub BUILD="build/$1" CFLAGS="$2" \
			DEBUG= LIBGPIOD_VERSION= CROSS_COMPILE=
	) > "$S/make-$1.log" 2>&1 || {
		sed 's/^/  /' "$S/make-$1.log" >&2
		return 1
	}
}

# The archive must stay limited to the interface under flags a packager
# or a user passes: -flto puts intermediate code into the objects, whose
# symbol table objcopy alone does not change, and a visibility of
# default makes every name of the library global to begin with.
for v in "stub-lto:-O2 -flto" "stub-visible:-O2 -fvisibility=default"; do
	d=${v%%:*}
	f=${v#*:}
	check "$d ($f): the stub builds" build_variant "$d" "$f"
	check "$d: the archive holds one object of the whole library" \
		archive_is_one_object "$d"
	check "$d: the archive defines only the public functions as global names" \
		archive_globals_are_api "$d"
	check "$d: a program with its own transmit() and board_init() links the archive" \
		static_consumer_runs "$d"
done

# stale_archive_rebuilt: a tree whose archive was made by an older
# Makefile, from the objects as they are, gets the limited archive from
# the next make. The stamp records the tools and options of the archive,
# so a Makefile that makes it differently writes another stamp, and the
# old archive does not count as up to date even though it is newer than
# every object.
stale_archive_rebuilt()
{
	d="$TREE/build/stub-stale"

	build_variant stub-stale "-O2" || return 1
	grep -q -e '--keep-global-symbol' "$d/.flags" || return 1
	rm -f "$d/librsswitch.a" &&
		ar rcs "$d/librsswitch.a" "$d/librsswitch.o" \
			"$d/boards/stub/stub.o" "$d/boards/gpiod/gpiod_config.o" &&
		echo 'gcc -O2' > "$d/.flags" &&
		touch "$d/librsswitch.a" || return 1
	build_variant stub-stale "-O2" || return 1
	archive_is_one_object stub-stale && archive_globals_are_api stub-stale
}
check "an archive made by an older Makefile is made again" \
	stale_archive_rebuilt

# build_programs BOARD: builds rsssend and rsswitchd against the library
# of BOARD, which the Makefiles of the examples make first.
build_programs()
{
	for d in basic network; do
		(
			unset MAKEFLAGS MFLAGS MAKELEVEL
			make -s -C "$TREE/examples/$d" BOARD="$1" DEBUG= \
				LIBGPIOD_VERSION= CROSS_COMPILE=
		) > "$S/make-prog-$1.log" 2>&1 || {
			sed 's/^/  /' "$S/make-prog-$1.log" >&2
			return 1
		}
	done
}

# program_needs FILE: FILE needs the SONAME of the library and no
# libgpiod of its own; the library brings libgpiod in, when it needs it.
program_needs()
{
	[ -x "$1" ] || return 1
	readelf -d "$1" |
		grep -q "NEEDED.*\[librsswitch\.so\.$MAJOR\]" || return 1
	! readelf -d "$1" | grep -q 'NEEDED.*libgpiod'
}

for b in $BOARDS; do
	check "$b: the programs build against the library" \
		build_programs "$b"
	check "$b: rsssend needs the library and no libgpiod" \
		program_needs "$TREE/examples/basic/rsssend"
	check "$b: rsswitchd needs the library and no libgpiod" \
		program_needs "$TREE/examples/network/rsswitchd"
	check "$b: rsssend has no RPATH or RUNPATH" \
		no_rpath "$TREE/examples/basic/rsssend"
	check "$b: rsswitchd has no RPATH or RUNPATH" \
		no_rpath "$TREE/examples/network/rsswitchd"
done

# The last program build above is the stub, but rebuild it so that the
# check does not depend on the order of BOARDS.
rsssend_runs()
{
	build_programs stub || return 1
	LD_LIBRARY_PATH="$TREE/build/stub" \
		"$TREE/examples/basic/rsssend" 1 17 23 1 \
		> "$S/rsssend.out" || return 1
	printf '%s\n' 0FFF0000F00F > "$S/rsssend.want"
	cmp -s "$S/rsssend.out" "$S/rsssend.want"
}
check "rsssend sends through the stub library and prints the codeword" \
	rsssend_runs

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

# BUILD moves the files of a board out of the build directory, with an
# absolute and with a relative path, and writes nothing to build/ then.
# make clean removes the directory again, and an empty BUILD stops make.
# The program links against the library in the other place, which is
# what the examples and the tests do on a file system without symbolic
# links.
ELSE="$S/elsewhere"
mkdir "$ELSE" || exit 1
cp -R "$TREE/Makefile" "$TREE/librsswitch.c" "$TREE/librsswitch.h" \
	"$TREE/librsswitch.map" "$TREE/boards" "$ELSE" || exit 1

make_else()
{
	(
		unset MAKEFLAGS MFLAGS MAKELEVEL
		make -s -C "$ELSE" BOARD=stub DEBUG= LIBGPIOD_VERSION= \
			CROSS_COMPILE= "$@"
	) > "$S/make-else.log" 2>&1 || {
		sed 's/^/  /' "$S/make-else.log" >&2
		return 1
	}
}

build_dir_absolute()
{
	make_else BUILD="$S/out/stub" || return 1
	[ -f "$S/out/stub/librsswitch.so.$VERSION" ] &&
		[ -f "$S/out/stub/librsswitch.a" ] &&
		[ "$(readlink "$S/out/stub/librsswitch.so")" = \
			"librsswitch.so.$MAJOR" ] &&
		[ ! -e "$ELSE/build" ]
}
check "BUILD with an absolute path builds there and not in build/" \
	build_dir_absolute

build_dir_relative()
{
	make_else BUILD=rel/stub || return 1
	[ -f "$ELSE/rel/stub/librsswitch.so.$VERSION" ] &&
		[ ! -e "$ELSE/build" ]
}
check "BUILD with a relative path is taken relative to the make directory" \
	build_dir_relative

build_dir_empty()
{
	! make_else BUILD= 2> /dev/null &&
		grep -q 'BUILD must not be empty' "$S/make-else.log"
}
check "an empty BUILD stops make" build_dir_empty

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

build_dir_linked()
{
	cc -Wall -Werror -I"$ELSE" -o "$S/consumer-else" "$S/consumer.c" \
		-L"$S/out/stub" -lrsswitch || return 1
	LD_LIBRARY_PATH="$S/out/stub" "$S/consumer-else" > "$S/else.out" ||
		return 1
	printf '%s\n%s\n' 0FFF0000F00F "$VERSION" > "$S/else.want"
	cmp -s "$S/else.out" "$S/else.want"
}
check "a program built against the library in BUILD runs" \
	build_dir_linked

build_dir_clean()
{
	make_else BUILD="$S/out/stub" clean || return 1
	make_else BUILD=rel/stub clean || return 1
	[ ! -e "$S/out/stub" ] && [ ! -e "$ELSE/rel/stub" ]
}
check "make clean removes the BUILD directory" build_dir_clean

echo "$passed/$total checks passed" >&2
[ "$passed" -eq "$total" ] || exit 1
