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
# for ever. It also checks make install with stand-ins for systemctl
# and the account tools, and with an install wrapper that keeps every
# write in a temporary directory, so it is safe for any user, root
# included. Takes the examples/network directory as $1, needs no root
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
check "[Unit] is ordered after network-online.target" \
	is_value Unit After "network.target network-online.target"
check "[Unit] wants network-online.target" \
	is_value Unit Wants network-online.target

# The install checks. Every tool that could reach the host is a stand-in
# in $S/bin, first in PATH: systemctl, getent, groupadd and useradd only
# log their call to $S/calls.log, and install refuses any absolute path
# outside $S before anything is written, whatever the user and the
# permissions. So make install runs below $S only, for root as well.
S=$(mktemp -d) || exit 1
trap 'rm -rf "$S"' EXIT
mkdir "$S/bin" "$S/build"
REAL_INSTALL=$(command -v install)
: > "$S/calls.log"

for tool in groupadd useradd systemctl; do
cat > "$S/bin/$tool" <<EOF
#!/bin/sh
echo "$tool \$*" >> "$S/calls.log"
exit 0
EOF
chmod +x "$S/bin/$tool"
done

# getent logs its call like the others. While $S/no-accounts exists it
# exits 2, as the real getent does for a name that is not there, so the
# host looks like one without the gpio group and the rsswitchd user.
# Otherwise it exits 0, and both accounts exist.
cat > "$S/bin/getent" <<EOF
#!/bin/sh
echo "getent \$*" >> "$S/calls.log"
[ ! -e "$S/no-accounts" ] || exit 2
exit 0
EOF
chmod +x "$S/bin/getent"

cat > "$S/bin/install" <<EOF
#!/bin/sh
for arg in "\$@"; do
	case \$arg in
	"$S"/*)
		;;
	/*)
		echo "install REFUSED \$arg" >> "$S/calls.log"
		exit 1
		;;
	esac
done
exec "$REAL_INSTALL" "\$@"
EOF
chmod +x "$S/bin/install"

printf '#!/bin/sh\necho v1\n' > "$S/build/rsswitchd"
chmod +x "$S/build/rsswitchd"
cp "$NET/rsswitchd.service" "$S/build/rsswitchd.service"

# shims_ok is set by shims_first. No make runs unless it is 1.
shims_ok=0

# shims_first: the five tools resolve into $S/bin.
shims_first()
{
	if [ -z "$REAL_INSTALL" ]; then
		echo "  install: no install program found" >&2
		return 1
	fi
	for tool in systemctl getent groupadd useradd install; do
		found=$(PATH="$S/bin:$PATH" command -v "$tool")
		if [ "$found" != "$S/bin/$tool" ]; then
			echo "  $tool resolves to ${found:-nothing}, not $S/bin/$tool" >&2
			return 1
		fi
	done
	shims_ok=1
}

# install_run ARGS...: make install of the daemon, with the stand-ins
# first in PATH. Returns the status of make, or 1 without running make
# when the stand-ins are not in place.
install_run()
{
	[ "$shims_ok" -eq 1 ] || return 1
	(
		unset MAKEFLAGS MFLAGS MAKELEVEL DESTDIR
		PATH="$S/bin:$PATH"
		make -s -f "$NET/Makefile" -C "$S/build" install "$@"
	) > "$S/make.log" 2>&1
}

# systemctl_calls: the systemctl calls made so far, one per line.
systemctl_calls()
{
	grep '^systemctl ' "$S/calls.log" 2>/dev/null
}

EXPECTED_CALLS="systemctl daemon-reload
systemctl enable rsswitchd
systemctl reset-failed rsswitchd
systemctl restart rsswitchd"

# restarts_service: the calls are daemon-reload, enable, reset-failed and
# restart, in this order, so that a failed start limit is cleared before
# a running daemon is replaced and a stopped one is started. Prints the
# actual calls when they differ.
restarts_service()
{
	if [ "$(systemctl_calls)" != "$EXPECTED_CALLS" ]; then
		echo "  systemctl calls were:" >&2
		systemctl_calls | sed 's/^/    /' >&2
		return 1
	fi
}

# no_account_tools: neither groupadd nor useradd was called, as on a
# host that already has the gpio group and the rsswitchd user.
no_account_tools()
{
	! grep -Eq '^(groupadd|useradd) ' "$S/calls.log"
}

# The calls of an install on a host with neither the group nor the
# user: look both up, create both, then the service calls.
EXPECTED_FRESH="getent group gpio
groupadd --system gpio
getent passwd rsswitchd
useradd --system --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin --groups gpio rsswitchd
systemctl daemon-reload
systemctl enable rsswitchd
systemctl reset-failed rsswitchd
systemctl restart rsswitchd"

# creates_accounts: the whole call log is EXPECTED_FRESH. Prints the
# actual calls when they differ.
creates_accounts()
{
	if [ "$(cat "$S/calls.log")" != "$EXPECTED_FRESH" ]; then
		echo "  calls were:" >&2
		sed 's/^/    /' "$S/calls.log" >&2
		return 1
	fi
}

# same_file SRC DEST: DEST is a file equal to SRC.
same_file()
{
	[ -f "$2" ] && cmp -s "$1" "$2"
}

# binary_equals SRC DEST: DEST is executable and equal to SRC.
binary_equals()
{
	[ -x "$2" ] && same_file "$1" "$2"
}

# staged_files_placed: the staged install put the binary and the unit
# under DESTDIR, at the default PREFIX and SYSTEMD_UNIT_DIR.
staged_files_placed()
{
	binary_equals "$S/build/rsswitchd" \
		"$S/stage/usr/local/bin/rsswitchd" &&
	same_file "$NET/rsswitchd.service" \
		"$S/stage/etc/systemd/system/rsswitchd.service"
}

# staged_calls_none: the staged install ran, and called none of the
# stand-ins for the account and service tools.
staged_calls_none()
{
	[ "$staged_rc" -eq 0 ] && [ ! -s "$S/calls.log" ]
}

unstaged_rc=1
upgrade_rc=1
fresh_rc=1
staged_rc=1

check "the five tools resolve into the stand-in directory" shims_first

install_run PREFIX="$S/prefix" SYSTEMD_UNIT_DIR="$S/units"
unstaged_rc=$?
check "install exits 0" test "$unstaged_rc" -eq 0
check "install puts the binary in place" \
	binary_equals "$S/build/rsswitchd" "$S/prefix/bin/rsswitchd"
check "install puts the unit in place" \
	same_file "$NET/rsswitchd.service" "$S/units/rsswitchd.service"
check "install runs daemon-reload, enable, reset-failed, restart" \
	restarts_service
check "install on a host with the accounts calls no groupadd or useradd" \
	no_account_tools

# An upgrade: a new build replaces the old binary and restarts the
# daemon, which is already installed and running.
printf '#!/bin/sh\necho v2\n' > "$S/build/rsswitchd"
: > "$S/calls.log"
install_run PREFIX="$S/prefix" SYSTEMD_UNIT_DIR="$S/units"
upgrade_rc=$?
check "upgrade exits 0" test "$upgrade_rc" -eq 0
check "upgrade installs the new binary" \
	binary_equals "$S/build/rsswitchd" "$S/prefix/bin/rsswitchd"
check "upgrade runs daemon-reload, enable, reset-failed, restart" \
	restarts_service
check "upgrade on a host with the accounts calls no groupadd or useradd" \
	no_account_tools

# A fresh host has neither the gpio group nor the rsswitchd user, and
# make install creates both before it starts the service.
: > "$S/no-accounts"
: > "$S/calls.log"
install_run PREFIX="$S/prefix" SYSTEMD_UNIT_DIR="$S/units"
fresh_rc=$?
rm -f "$S/no-accounts"
check "fresh host install exits 0" test "$fresh_rc" -eq 0
check "fresh host install creates the group and the user" creates_accounts

# A staged install touches no account and no service.
: > "$S/calls.log"
install_run DESTDIR="$S/stage"
staged_rc=$?
check "staged install exits 0" test "$staged_rc" -eq 0
check "staged install places the binary and the unit under DESTDIR" \
	staged_files_placed
check "staged install calls none of the stand-ins" \
	staged_calls_none

echo "$passed/$total checks passed" >&2
[ "$passed" -eq "$total" ] || exit 1
