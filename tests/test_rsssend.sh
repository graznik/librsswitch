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

# --- commit 2: strtoul validation, one bad argument at a time -------
check "invalid dev" 2 "" contains "invalid DEV 'abc'"  abc 0 0 1
check "invalid group" 2 "" contains "invalid GROUP 'abc'"  1 abc 0 1
check "invalid socket" 2 "" contains "invalid SOCKET 'x'"  1 0 x 1
check "invalid data" 2 "" contains "invalid DATA 'y'"  1 0 0 y
check "empty group" 2 "" contains "invalid GROUP ''"  1 "" 0 1
check "leading minus" 2 "" contains "invalid GROUP '-1'"  1 -1 0 1
check "leading plus" 2 "" contains "invalid GROUP '+1'"  1 +1 0 1
check "leading space" 2 "" contains "invalid GROUP"  1 " 1" 0 1
check "trailing space" 2 "" contains "invalid GROUP"  1 "1 " 0 1
check "trailing garbage" 2 "" contains "invalid GROUP '17x'"  1 17x 0 1
check "hex" 2 "" contains "invalid GROUP '0x11'"  1 0x11 0 1
check "overflow uint32" 2 "" contains "invalid GROUP"  1 4294967296 0 1
check "overflow huge" 2 "" contains "invalid GROUP"  1 99999999999999999999 0 1
check "dev out of range" 2 "" contains "invalid DEV '2'"  2 0 0 1
check "data out of range" 2 "" contains "invalid DATA '2'"  1 0 0 2

# --- commit 2: still the same codewords for valid input -------------
check "rev off group2 socket1" 0 "FF1FF1F00010" empty ""  0 2 1 1
check "pollin max group/socket" 0 "0000000000F0" empty ""  1 31 31 0
check "rev on group3 socket2" 0 "FFF1FF100001" empty ""  0 3 2 0
check "leading zeros" 0 "0FFF0000F00F" empty ""  1 017 023 1
check "rev group out of range" 1 "" contains "rsssend:"  0 4 0 1
check "pollin group uint32-max" 1 "" contains "rsssend:"  1 4294967295 0 1

# --- commit 2: left-to-right order, --gpio only at positions 5/6 ----
check "first bad arg names DEV" 2 "" contains "invalid DEV"  abc def 0 1
check "first bad arg not GROUP" 2 "" not-contains "GROUP"  abc def 0 1
check "gpio option accepted" 0 "0FFF0000F00F" empty ""  1 17 23 1 --gpio x
check "gpio missing value" 2 "" any ""  1 17 23 1 --gpio
check "trailing extra arg" 2 "" any ""  1 17 23 1 extra
check "gpio plus extra arg" 2 "" any ""  1 17 23 1 --gpio x extra
check "unknown option" 2 "" any ""  1 17 23 1 --verbose x

# --- edge cases: no arguments / empty-string sole argument ----------
check "empty string sole argument" 2 "" any ""  ""

echo "$passed/$total checks passed" >&2

[ "$passed" -eq "$total" ] || exit 1
exit 0
