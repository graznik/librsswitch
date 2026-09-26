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

# Exercises rsssend's argument handling (CLI-01, D-20..D-22): usage or
# parse errors exit 2 with the diagnostic on stderr and nothing on
# stdout, an address the library rejects exits 1 with nothing on
# stdout, and valid input still produces the exact codeword on stdout
# with an empty stderr. Takes the path to the rsssend binary as $1;
# run by `make test`.

BIN=$1
if [ -z "$BIN" ]; then
	echo "usage: $0 <path-to-rsssend>" >&2
	exit 2
fi

ERRFILE=$(mktemp)
trap 'rm -f "$ERRFILE"' EXIT

total=0
passed=0

# check DESC EXPECTED_STATUS EXPECTED_STDOUT STDERR_MODE STDERR_TEXT ARGS...
#   STDERR_MODE: empty | contains | not-contains | any
check()
{
	desc=$1
	exp_status=$2
	exp_stdout=$3
	mode=$4
	text=$5
	shift 5

	out=$("$BIN" "$@" 2>"$ERRFILE")
	status=$?
	err=$(cat "$ERRFILE")

	ok=1
	[ "$status" -eq "$exp_status" ] || ok=0
	[ "$out" = "$exp_stdout" ] || ok=0

	case $mode in
	empty)
		[ -z "$err" ] || ok=0
		;;
	contains)
		printf '%s\n' "$err" | grep -qF -- "$text" || ok=0
		;;
	not-contains)
		if printf '%s\n' "$err" | grep -qF -- "$text"; then
			ok=0
		fi
		;;
	any)
		:
		;;
	esac

	total=$((total + 1))
	if [ "$ok" -eq 1 ]; then
		passed=$((passed + 1))
	else
		echo "FAIL $desc (status=$status want=$exp_status" \
		     "stdout='$out' want='$exp_stdout' stderr='$err')" >&2
	fi
}

# --- commit 1: usage to stderr, distinct exit codes -----------------
check "no arguments" 2 "" contains "Usage:"
check "valid pollin on" 0 "0FFF0000F00F" empty ""  1 17 23 1
check "library-rejected group" 1 "" contains "rsssend:"  1 32 0 1

echo "$passed/$total checks passed" >&2

[ "$passed" -eq "$total" ] || exit 1
exit 0
