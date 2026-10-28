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

# Checks the abicheck and abi-baseline goals of the top-level Makefile:
# the unchanged library passes, an added function and a changed
# parameter type fail, a library without debug information is refused,
# a machine without a baseline is skipped, and a new baseline makes the
# changed library pass. Every case works on a fresh copy of the sources
# below the temporary directory, so the checkout is not written to.
# Takes the root of the code repository as $1; run by `make test` only
# when abidiff and a baseline for this machine exist, so both are
# looked for here as well, for a run by hand.

if [ -z "$1" ] || [ ! -f "$1/tests/Makefile" ]; then
	echo "usage: $0 <code repository root>" >&2
	exit 2
fi
ROOT=$(cd "$1" && pwd -P)

if ! command -v abidiff > /dev/null 2>&1; then
	echo "SKIP abicheck tests: abidiff not found" >&2
	exit 0
fi

MACHINE=$(cc -dumpmachine | cut -d- -f1)
if [ ! -f "$ROOT/abi/$MACHINE.abi" ]; then
	echo "SKIP abicheck tests: no ABI baseline for $MACHINE" >&2
	exit 0
fi

# The tests build in the default directory of a copy. A BUILD in the
# environment of the caller would move the libraries away from it.
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

# fresh NAME: a copy of the sources that the goals need in $S/NAME.
fresh()
{
	mkdir "$S/$1" || return 1
	cp -R "$ROOT/Makefile" "$ROOT/librsswitch.c" "$ROOT/librsswitch.h" \
		"$ROOT/librsswitch.map" "$ROOT/librsswitch.pc.in" \
		"$ROOT/boards" "$ROOT/abi" "$S/$1"
}

# run_goal NAME BOARD GOAL: makes GOAL in the copy NAME for BOARD, its
# output in $S/out.txt. The knobs are emptied, so a DEBUG or
# CROSS_COMPILE in the environment of the caller does not change what
# is checked. Returns the status of make.
run_goal()
{
	(
		unset MAKEFLAGS MFLAGS MAKELEVEL
		make -s -C "$S/$1" BOARD="$2" DEBUG= LIBGPIOD_VERSION= \
			CROSS_COMPILE= "$3"
	) > "$S/out.txt" 2>&1
}

# says TEXT: the output of the last goal holds TEXT.
says()
{
	grep -qF -- "$1" "$S/out.txt"
}

# The copy of the unchanged sources is the reference: it passes.
unchanged_passes()
{
	fresh same || return 1
	run_goal same stub abicheck && says 'ABI unchanged'
}
check "abicheck passes for the unchanged library" unchanged_passes

# The libgpiod board has the same interface as the stub board.
have_gpiod()
{
	printf '%s\n' '#include <gpiod.h>' \
		'int main(void) { return 0; }' > "$S/probe.c" &&
		cc $CPPFLAGS "$S/probe.c" $LDFLAGS -lgpiod \
			-o "$S/probe" > /dev/null 2>&1
}

gpiod_passes()
{
	fresh gpiod || return 1
	run_goal gpiod gpiod abicheck && says 'ABI unchanged'
}
if have_gpiod; then
	check "abicheck passes for the libgpiod board" gpiod_passes
else
	echo "SKIP gpiod board: gpiod.h or libgpiod not found" >&2
fi

# A function added to the header and the library changes the interface.
added_function()
{
	fresh added || return 1
	sed -i 's/^\(RSS_API const char \*rss_version(void);\)$/\1\n\nRSS_API int32_t rss_added(void);/' \
		"$S/added/librsswitch.h" || return 1
	printf '\nint32_t rss_added(void)\n{\n\treturn 0;\n}\n' \
		>> "$S/added/librsswitch.c" || return 1
	grep -q 'rss_added' "$S/added/librsswitch.h" || return 1

	! run_goal added stub abicheck && says 'the ABI differs'
}
check "abicheck fails for an added function" added_function

# A parameter of another type changes the interface as well.
changed_parameter()
{
	fresh param || return 1
	sed -i 's/rss_send_codeword(const char \*tristate, uint32_t pulse_len)/rss_send_codeword(const char *tristate, uint16_t pulse_len)/' \
		"$S/param/librsswitch.h" "$S/param/librsswitch.c" || return 1
	grep -q 'uint16_t pulse_len' "$S/param/librsswitch.h" &&
		grep -q 'uint16_t pulse_len' "$S/param/librsswitch.c" ||
		return 1

	! run_goal param stub abicheck && says 'the ABI differs'
}
check "abicheck fails for a changed parameter type" changed_parameter

# Without debug information abidiff would compare the symbols only and
# pass a changed type, so the library is refused.
stripped_refused()
{
	fresh strip || return 1
	run_goal strip stub all || return 1
	strip --strip-debug "$S"/strip/build/stub/librsswitch.so.* ||
		return 1

	! run_goal strip stub abicheck && says 'no debug info'
}
check "abicheck refuses a library without debug information" \
	stripped_refused

# Another machine has no baseline: one line, and success.
no_baseline_skips()
{
	fresh nobase || return 1
	rm -f "$S/nobase/abi/$MACHINE.abi" || return 1

	run_goal nobase stub abicheck &&
		says 'SKIP abicheck: no ABI baseline' &&
		[ "$(wc -l < "$S/out.txt")" -eq 1 ]
}
check "abicheck skips a machine without a baseline" no_baseline_skips

# A baseline made from the changed library accepts it.
new_baseline_accepts()
{
	[ -d "$S/added" ] || return 1
	run_goal added stub abi-baseline || return 1
	run_goal added stub abicheck && says 'ABI unchanged'
}
check "abi-baseline makes the changed library pass" new_baseline_accepts

echo "$passed/$total checks passed" >&2
[ "$passed" -eq "$total" ] || exit 1
