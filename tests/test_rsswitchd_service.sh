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

# Checks how rsswitchd is run as a service from examples/network,
# starting with the start limit of rsswitchd.service: a start that
# always fails must end in the failed state instead of being retried
# for ever. Takes the examples/network directory as $1, needs no root
# and no system manager; run by `make test`.

if [ -z "$1" ] || [ ! -f "$1/rsswitchd.service" ]; then
	echo "usage: $0 <examples/network directory>" >&2
	exit 2
fi
NET=$(cd "$1" && pwd)

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

# unit_values SECTION KEY: prints the value of every KEY= directive in
# [SECTION] of rsswitchd.service, one per line.
unit_values()
{
	awk -v sec="$1" -v key="$2" '
		/^[ \t]*#/ { next }
		/^\[/ { cur = substr($0, 2, length($0) - 2); next }
		cur == sec && index($0, key "=") == 1 {
			print substr($0, length(key) + 2)
		}
	' "$NET/rsswitchd.service"
}

# bare_start_limit: prints every StartLimit line that has no comment
# line directly above it.
bare_start_limit()
{
	awk '
		/^StartLimit/ && prev !~ /^#/ { print }
		{ prev = $0 }
	' "$NET/rsswitchd.service"
}

# is_value SECTION KEY VALUE: KEY= appears once in SECTION, as VALUE.
is_value()
{
	[ "$(unit_values "$1" "$2")" = "$3" ]
}

# no_start_limit_in_service: [Service] holds no start limit directive;
# systemd reads them from [Unit] only.
no_start_limit_in_service()
{
	[ -z "$(unit_values Service StartLimitIntervalSec)" ] &&
	[ -z "$(unit_values Service StartLimitBurst)" ] &&
	[ -z "$(unit_values Service StartLimitInterval)" ]
}

# restart_policy: [Service] restarts after a failure, after 2 s.
restart_policy()
{
	is_value Service Restart on-failure &&
	is_value Service RestartSec 2
}

# digits VALUE: VALUE is a non-empty string of digits.
digits()
{
	case $1 in
	"" | *[!0-9]*)
		return 1
		;;
	esac
}

# Seconds that a failing start of the daemon may take before it exits.
# The same rule and number as examples/web/tests/test_service_unit.py.
SLOW_FAILED_START=8

# start_limit_reachable: all the starts of the burst fit into the
# interval. systemd's own default of 5 starts within 10 s is never
# reached when every restart waits RestartSec, so without a longer
# interval a start that always fails would be retried for ever.
start_limit_reachable()
{
	interval=$(unit_values Unit StartLimitIntervalSec)
	burst=$(unit_values Unit StartLimitBurst)
	restart_sec=$(unit_values Service RestartSec)

	digits "$interval" && digits "$burst" && digits "$restart_sec" ||
		return 1
	[ $((burst * (restart_sec + SLOW_FAILED_START))) -le "$interval" ]
}

# no_bare_start_limit: every StartLimit line has a comment above it.
no_bare_start_limit()
{
	[ -z "$(bare_start_limit)" ]
}

check "[Unit] has StartLimitIntervalSec=60" \
	is_value Unit StartLimitIntervalSec 60
check "[Unit] has StartLimitBurst=5" \
	is_value Unit StartLimitBurst 5
check "[Service] has no StartLimit directive" \
	no_start_limit_in_service
check "[Service] has Restart=on-failure and RestartSec=2" \
	restart_policy
check "start limit is reachable with RestartSec" \
	start_limit_reachable
check "every StartLimit line has a comment above it" \
	no_bare_start_limit

echo "$passed/$total checks passed" >&2
[ "$passed" -eq "$total" ] || exit 1
