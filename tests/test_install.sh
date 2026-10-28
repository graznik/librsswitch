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

# Checks make install and make uninstall of the top-level Makefile:
# that exactly six paths are installed with the right modes and links,
# that PREFIX, LIBDIR and DESTDIR are honoured, that ldconfig runs only
# without DESTDIR and only warns when it fails, that the stub and the
# libgpiod board replace each other, that one make call builds and
# installs from a tree without build/, that an install after a build
# rebuilds nothing, and that a program builds and runs against the
# installed tree through pkg-config. Takes the root of the code
# repository as $1; run by `make test`. It needs no root.
#
# Nothing is written outside a temporary directory. The library is
# built in a copy of the sources there, and install, ln, rm and
# ldconfig are stand-ins first in PATH: the first three refuse an
# absolute argument outside the temporary directory, an argument with
# "..", and a target-directory option, and then run the real tool;
# ldconfig only logs its call. Other tools are not wrapped, so a recipe
# line that wrote with another tool would not be caught; the install and
# uninstall recipes use only install, ln, rm, sed and ldconfig.

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
mkdir "$S/bin"
: > "$S/calls.log"

header_number()
{
	awk -v n="LIBRSSWITCH_VERSION_$1" '$2 == n { print $3 }' \
		"$ROOT/librsswitch.h"
}
MAJOR=$(header_number MAJOR)
MINOR=$(header_number MINOR)
PATCH=$(header_number PATCH)
VERSION="$MAJOR.$MINOR.$PATCH"

# The stand-ins. REAL_* are the tools they run after the checks.
for tool in install ln rm; do
	real=$(command -v "$tool")
	if [ -z "$real" ]; then
		echo "FAIL no $tool program found" >&2
		exit 1
	fi
	cat > "$S/bin/$tool" <<EOF
#!/bin/sh
for arg in "\$@"; do
	case \$arg in
	*/../*|*/..|-t*|-[!-]*t*|--target-directory*)
		echo "$tool REFUSED \$arg" >> "$S/calls.log"
		exit 1
		;;
	"$S"/*)
		;;
	/*)
		echo "$tool REFUSED \$arg" >> "$S/calls.log"
		exit 1
		;;
	esac
done
exec "$real" "\$@"
EOF
	chmod +x "$S/bin/$tool"
done

# ldconfig logs its call and exits with the status in $S/ldconfig-status
# when that file exists, else with 0.
cat > "$S/bin/ldconfig" <<EOF
#!/bin/sh
echo "ldconfig \$*" >> "$S/calls.log"
[ ! -f "$S/ldconfig-status" ] || exit \$(cat "$S/ldconfig-status")
exit 0
EOF
chmod +x "$S/bin/ldconfig"

# shims_first: the four tools resolve into $S/bin.
shims_first()
{
	for tool in install ln rm ldconfig; do
		found=$(PATH="$S/bin:$PATH" command -v "$tool")
		if [ "$found" != "$S/bin/$tool" ]; then
			echo "  $tool resolves to ${found:-nothing}, not $S/bin/$tool" >&2
			return 1
		fi
	done
}

# stand_ins_ok is set from shims_first. No make runs unless it is 1.
stand_ins_ok=0
shims_first && stand_ins_ok=1

# mk ARGS...: make with the stand-ins first in PATH and the knobs that
# change the build emptied, in the copy of the sources. Returns the
# status of make, or 1 without running make when the stand-ins are not
# in place. The output is in $S/make.log.
mk()
{
	[ "$stand_ins_ok" -eq 1 ] || return 1
	(
		unset MAKEFLAGS MFLAGS MAKELEVEL DESTDIR PREFIX LIBDIR \
			INCLUDEDIR PKGCONFIGDIR BOARD
		PATH="$S/bin:$PATH"
		make -s -C "$S/tree" DEBUG= LIBGPIOD_VERSION= CROSS_COMPILE= \
			"$@"
	) > "$S/make.log" 2>&1
}

# The stand-ins refuse what they should. The calls of this check are
# not part of the later ones.
refuses_outside()
{
	[ "$stand_ins_ok" -eq 1 ] || return 1
	(
		PATH="$S/bin:$PATH"
		! rm -f /nonexistent-dir-of-the-test/file 2> /dev/null &&
		! ln -sf a /nonexistent-dir-of-the-test/link 2> /dev/null &&
		! install -D -m 0644 "$S/bin/ldconfig" "$S/../x" \
			2> /dev/null &&
		! install -t "$S" "$S/bin/ldconfig" 2> /dev/null
	) || return 1
	[ "$(grep -c REFUSED "$S/calls.log")" -eq 4 ]
}
check "the stand-ins refuse paths outside the temporary directory" \
	refuses_outside
: > "$S/calls.log"

# The sources the install needs, in a copy without build/.
mkdir "$S/tree" || exit 1
cp -R "$ROOT/Makefile" "$ROOT/librsswitch.c" "$ROOT/librsswitch.h" \
	"$ROOT/librsswitch.map" "$ROOT/librsswitch.pc.in" \
	"$ROOT/boards" "$S/tree" || exit 1

have_gpiod()
{
	printf '%s\n' '#include <gpiod.h>' \
		'int main(void) { return 0; }' > "$S/probe.c" &&
		cc $CPPFLAGS "$S/probe.c" $LDFLAGS -lgpiod \
			-o "$S/probe" > /dev/null 2>&1
}
GPIOD=no
if have_gpiod; then
	GPIOD=yes
else
	echo "SKIP gpiod board: gpiod.h or libgpiod not found" >&2
fi

# installed_list INC LIB PKG: the six paths of an install, sorted.
installed_list()
{
	printf '%s\n' "$1/librsswitch.h" "$2/librsswitch.so.$VERSION" \
		"$2/librsswitch.so.$MAJOR" "$2/librsswitch.so" \
		"$2/librsswitch.a" "$3/librsswitch.pc" | LC_ALL=C sort
}

# files_are DIR INC LIB PKG: DIR holds, below it, exactly the six
# paths of an install and no other file or link.
files_are()
{
	[ -d "$1" ] &&
		[ "$(find "$1" ! -type d | LC_ALL=C sort)" = \
			"$(installed_list "$2" "$3" "$4")" ]
}

# default_layout DIR: files_are for the default PREFIX /usr/local.
default_layout()
{
	files_are "$1" "$1/usr/local/include" "$1/usr/local/lib" \
		"$1/usr/local/lib/pkgconfig"
}

# One make call builds and installs from a tree without build/. The
# tree is built by the first of these calls and stays built.
clean_tree_stub()
{
	[ ! -e "$S/tree/build" ] || return 1
	mk BOARD=stub install DESTDIR="$S/clean-stub" || {
		sed 's/^/  /' "$S/make.log" >&2
		return 1
	}
	[ -f "$S/tree/build/stub/librsswitch.so.$VERSION" ] &&
		default_layout "$S/clean-stub"
}
check "BOARD=stub install builds and installs in one call" clean_tree_stub

clean_tree_gpiod()
{
	mk BOARD=gpiod install DESTDIR="$S/clean-gpiod" || {
		sed 's/^/  /' "$S/make.log" >&2
		return 1
	}
	[ -f "$S/tree/build/gpiod/librsswitch.so.$VERSION" ] &&
		default_layout "$S/clean-gpiod"
}
[ "$GPIOD" = no ] || check "BOARD=gpiod install builds and installs in one call" \
	clean_tree_gpiod

# After a build, an install rebuilds nothing: nothing below build/ is
# newer than the marker except the regenerated librsswitch.pc. Directories
# are left out, because rewriting the file changes the time of its own.
install_rebuilds_nothing()
{
	[ -d "$S/tree/build/stub" ] || return 1
	touch "$S/marker"
	mk BOARD=stub install DESTDIR="$S/again" || return 1
	newer=$(find "$S/tree/build/stub" ! -type d -newer "$S/marker" \
		! -name librsswitch.pc)
	[ -z "$newer" ] || printf '  rewritten: %s\n' "$newer" >&2
	[ -z "$newer" ] && default_layout "$S/again"
}
check "an install after a build rebuilds nothing" install_rebuilds_nothing

# mode_of FILE: the permission bits of FILE, in octal.
mode_of()
{
	stat -c %a "$1"
}

# stub_stage: a stage with a default stub install, made once.
stub_stage()
{
	if [ ! -d "$S/stage" ]; then
		mk BOARD=stub install DESTDIR="$S/stage" || return 1
	fi
}
stub_stage

stage_layout()
{
	default_layout "$S/stage"
}
check "a staged stub install writes exactly six paths" stage_layout

stage_modes()
{
	L="$S/stage/usr/local/lib"
	[ "$(mode_of "$S/stage/usr/local/include/librsswitch.h")" = 644 ] &&
		[ "$(mode_of "$L/librsswitch.so.$VERSION")" = 755 ] &&
		[ "$(mode_of "$L/librsswitch.a")" = 644 ] &&
		[ "$(mode_of "$L/pkgconfig/librsswitch.pc")" = 644 ]
}
check "the installed files have the right modes" stage_modes

stage_links()
{
	L="$S/stage/usr/local/lib"
	[ "$(readlink "$L/librsswitch.so.$MAJOR")" = \
		"librsswitch.so.$VERSION" ] &&
		[ "$(readlink "$L/librsswitch.so")" = \
			"librsswitch.so.$MAJOR" ]
}
check "the installed links are relative and point to the right names" \
	stage_links

# A staged install does not refresh the loader cache.
no_ldconfig_with_destdir()
{
	! grep -q '^ldconfig' "$S/calls.log"
}
check "ldconfig is not called when DESTDIR is set" no_ldconfig_with_destdir

stage_pc_stub()
{
	pc="$S/stage/usr/local/lib/pkgconfig/librsswitch.pc"
	grep -qx 'prefix=/usr/local' "$pc" &&
		grep -qx 'libdir=/usr/local/lib' "$pc" &&
		grep -qx 'includedir=/usr/local/include' "$pc" &&
		grep -qx 'Name: librsswitch' "$pc" &&
		grep -qx "Version: $VERSION" "$pc" &&
		grep -qx 'Libs: -L${libdir} -lrsswitch' "$pc" &&
		grep -qx 'Cflags: -I${includedir}' "$pc" &&
		! grep -q 'Requires.private' "$pc"
}
check "the stub's librsswitch.pc has the version and no Requires.private" \
	stage_pc_stub

# A program built with pkg-config against the installed tree. The two
# variables are given to the pkg-config call itself: the first points it
# at the stage, the second prefixes the paths it prints. --static is not
# used, because the sysroot would also move the paths of libgpiod.
printf '%s\n' '#include <librsswitch.h>' \
	'' \
	'int main(void)' \
	'{' \
	'	return rss_send(RSS_POLLIN_2605, 17, 23, 1) == 0 ? 0 : 1;' \
	'}' > "$S/consumer.c"

# pc DIR ARGS...: pkg-config for the install below DIR.
pc()
{
	d=$1
	shift
	PKG_CONFIG_PATH="$d/usr/local/lib/pkgconfig" \
		PKG_CONFIG_SYSROOT_DIR="$d" pkg-config "$@"
}

pkg_modversion()
{
	[ "$(pc "$S/stage" --modversion librsswitch)" = "$VERSION" ]
}

pkg_consumer_runs()
{
	flags=$(pc "$S/stage" --cflags --libs librsswitch) || return 1
	# The flags are words of the command line on purpose.
	# shellcheck disable=SC2086
	cc -Wall -Werror -o "$S/consumer" "$S/consumer.c" $flags || return 1
	out=$(LD_LIBRARY_PATH="$S/stage/usr/local/lib" "$S/consumer") ||
		return 1
	[ "$out" = 0FFF0000F00F ]
}

pkg_consumer_needed()
{
	[ -x "$S/consumer" ] || return 1
	readelf -d "$S/consumer" |
		grep -q "NEEDED.*\[librsswitch\.so\.$MAJOR\]" &&
		! readelf -d "$S/consumer" | grep -Eq 'RPATH|RUNPATH'
}

if command -v pkg-config > /dev/null 2>&1; then
	check "pkg-config reports the version of the installed library" \
		pkg_modversion
	check "a program built with pkg-config runs against the install" \
		pkg_consumer_runs
	check "the program needs the SONAME and has no RPATH or RUNPATH" \
		pkg_consumer_needed
else
	echo "SKIP pkg-config consumer: pkg-config not found" >&2
fi

# The libgpiod board writes the dependency line, and a stub install over
# it into the same stage replaces the library, and drops the line and
# the dependency on libgpiod.
gpiod_then_stub()
{
	L="$S/swap/usr/local/lib"
	pc="$L/pkgconfig/librsswitch.pc"

	mk BOARD=gpiod install DESTDIR="$S/swap" || return 1
	default_layout "$S/swap" || return 1
	grep -qx 'Requires.private: libgpiod' "$pc" || return 1
	readelf -d "$L/librsswitch.so.$VERSION" |
		grep -q 'NEEDED.*libgpiod' || return 1

	mk BOARD=stub install DESTDIR="$S/swap" || return 1
	default_layout "$S/swap" || return 1
	! grep -q 'Requires.private' "$pc" &&
		! readelf -d "$L/librsswitch.so.$VERSION" |
			grep -q 'NEEDED.*libgpiod'
}
[ "$GPIOD" = no ] || check "the stub replaces a libgpiod install and drops its dependency" \
	gpiod_then_stub

# PREFIX and LIBDIR move the files, and the .pc says where they are.
prefix_layout()
{
	mk BOARD=stub install DESTDIR="$S/usr" PREFIX=/usr \
		LIBDIR=/usr/lib/multiarch-dir || return 1
	files_are "$S/usr" "$S/usr/usr/include" \
		"$S/usr/usr/lib/multiarch-dir" \
		"$S/usr/usr/lib/multiarch-dir/pkgconfig" &&
		grep -qx 'prefix=/usr' \
			"$S/usr/usr/lib/multiarch-dir/pkgconfig/librsswitch.pc" &&
		grep -qx 'libdir=/usr/lib/multiarch-dir' \
			"$S/usr/usr/lib/multiarch-dir/pkgconfig/librsswitch.pc" &&
		grep -qx 'includedir=/usr/include' \
			"$S/usr/usr/lib/multiarch-dir/pkgconfig/librsswitch.pc"
}
check "PREFIX and LIBDIR move the files and the .pc follows" prefix_layout

# INCLUDEDIR and PKGCONFIGDIR can be given separately too.
separate_dirs()
{
	mk BOARD=stub install DESTDIR="$S/sep" PREFIX=/opt/rss \
		INCLUDEDIR=/opt/inc PKGCONFIGDIR=/opt/pc || return 1
	files_are "$S/sep" "$S/sep/opt/inc" "$S/sep/opt/rss/lib" \
		"$S/sep/opt/pc"
}
check "INCLUDEDIR and PKGCONFIGDIR are honoured" separate_dirs

# Without DESTDIR the loader cache is refreshed, once, and a failing
# ldconfig only warns. PREFIX is below the temporary directory, which
# the stand-ins allow.
ldconfig_calls()
{
	grep -c '^ldconfig' "$S/calls.log"
}

ldconfig_once()
{
	before=$(ldconfig_calls)
	mk BOARD=stub install PREFIX="$S/real" || return 1
	[ "$(($(ldconfig_calls) - before))" -eq 1 ] &&
		files_are "$S/real" "$S/real/include" "$S/real/lib" \
			"$S/real/lib/pkgconfig"
}
check "install without DESTDIR calls ldconfig once" ldconfig_once

ldconfig_failure_warns()
{
	before=$(ldconfig_calls)
	echo 1 > "$S/ldconfig-status"
	mk BOARD=stub install PREFIX="$S/real2"
	status=$?
	rm -f "$S/ldconfig-status"
	[ "$status" -eq 0 ] &&
		[ "$(($(ldconfig_calls) - before))" -eq 1 ] &&
		grep -q "ldconfig.*$S/real2/lib" "$S/make.log"
}
check "a failing ldconfig only warns and names the library directory" \
	ldconfig_failure_warns

# uninstall removes the six paths and nothing else.
uninstall_removes_six()
{
	mk BOARD=stub install DESTDIR="$S/un" || return 1
	echo keep > "$S/un/usr/local/lib/unrelated.txt"
	echo keep > "$S/un/usr/local/include/unrelated.h"
	mk BOARD=stub uninstall DESTDIR="$S/un" || return 1
	[ "$(find "$S/un" ! -type d | LC_ALL=C sort)" = \
"$S/un/usr/local/include/unrelated.h
$S/un/usr/local/lib/unrelated.txt" ]
}
check "uninstall removes exactly the six paths" uninstall_removes_six

uninstall_without_install()
{
	mk BOARD=stub uninstall DESTDIR="$S/never" &&
		[ -z "$(find "$S/never" ! -type d 2> /dev/null)" ]
}
check "uninstall without an install is not an error" \
	uninstall_without_install

uninstall_ldconfig()
{
	before=$(ldconfig_calls)
	mk BOARD=stub uninstall PREFIX="$S/real" || return 1
	[ "$(($(ldconfig_calls) - before))" -eq 1 ] &&
		[ -z "$(find "$S/real" ! -type d)" ]
}
check "uninstall without DESTDIR removes the files and calls ldconfig" \
	uninstall_ldconfig

# The recipes never wrote outside the temporary directory.
no_refused_calls()
{
	if grep -q REFUSED "$S/calls.log"; then
		grep REFUSED "$S/calls.log" >&2
		return 1
	fi
}
check "no stand-in refused an argument" no_refused_calls

echo "$passed/$total checks passed" >&2
[ "$passed" -eq "$total" ] || exit 1
