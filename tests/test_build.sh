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

# Checks the build and test plumbing of the Makefiles offline, without
# root and without network: the venv target of tests/Makefile, with
# stand-ins for python3 and pip. Takes the root of the code repository
# as $1; run by `make test`.

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

# Every tool that could reach the host or the network is a stand-in in
# $S/bin, first in PATH, and every venv lives below $S. The stand-in
# python3 makes a fake venv; its python fails the pytest import, and its
# pip only logs its call. Both log to $S/calls.log.
S=$(mktemp -d) || exit 1
trap 'rm -rf "$S"' EXIT
trap 'exit 1' HUP INT TERM
mkdir "$S/bin"
: > "$S/calls.log"

cat > "$S/bin/python3" <<EOT
#!/bin/sh
echo "python3 \$*" >> "$S/calls.log"
if [ "\$1" = -m ] && [ "\$2" = venv ]; then
	mkdir -p "\$3/bin" || exit 1
	printf '#!/bin/sh\nexit 1\n' > "\$3/bin/python"
	printf '#!/bin/sh\necho "pip \$*" >> "$S/calls.log"\n' > "\$3/bin/pip"
	chmod +x "\$3/bin/python" "\$3/bin/pip"
fi
exit 0
EOT

# A pip on PATH is never meant to run: the venv target uses the pip of
# the venv. If it does run, the log shows it.
cat > "$S/bin/pip" <<EOT
#!/bin/sh
echo "stray pip \$*" >> "$S/calls.log"
exit 0
EOT
chmod +x "$S/bin/python3" "$S/bin/pip"

# stand_ins_ok is set by stand_ins_first. No make runs unless it is 1.
stand_ins_ok=0

# stand_ins_first: python3 and pip resolve into $S/bin.
stand_ins_first()
{
	for tool in python3 pip; do
		found=$(PATH="$S/bin:$PATH" command -v "$tool")
		if [ "$found" != "$S/bin/$tool" ]; then
			echo "  $tool resolves to ${found:-nothing}, not $S/bin/$tool" >&2
			return 1
		fi
	done
	stand_ins_ok=1
}
stand_ins_first

# make_venv DIR: make venv with WEB_VENV=DIR and the stand-ins first in
# PATH. Returns the status of make, or 1 without running make when the
# stand-ins are not in place.
make_venv()
{
	[ "$stand_ins_ok" -eq 1 ] || return 1
	(
		unset MAKEFLAGS MFLAGS MAKELEVEL
		PATH="$S/bin:$PATH"
		make -s -C "$ROOT/tests" venv WEB_VENV="$1" GPIOD_EXPECT_VERSION=
	) > "$S/make.log" 2>&1
}

# venv_calls DIR: the calls that making the venv DIR must log.
venv_calls()
{
	echo "python3 -m venv $1"
	echo "pip install -r $ROOT/examples/web/requirements-dev.txt"
}

# calls_are TEXT: the calls logged so far are exactly TEXT.
calls_are()
{
	[ "$(cat "$S/calls.log")" = "$1" ]
}

# A directory that is not there is made, and the requirements follow.
venv_made()
{
	: > "$S/calls.log"
	make_venv "$S/v1" || return 1
	calls_are "$(venv_calls "$S/v1")"
}
check "venv is made when the directory is missing" venv_made

# A python without a pip is what an interrupted `python3 -m venv` leaves
# behind. The directory is removed (the marker file goes with it) and
# made again.
venv_remade()
{
	mkdir -p "$S/v2/bin" || return 1
	printf '#!/bin/sh\nexit 1\n' > "$S/v2/bin/python"
	chmod +x "$S/v2/bin/python"
	: > "$S/v2/marker"
	: > "$S/calls.log"
	make_venv "$S/v2" || return 1
	[ ! -e "$S/v2/marker" ] || return 1
	calls_are "$(venv_calls "$S/v2")"
}
check "venv without pip is removed and made again" venv_remade

# A venv whose python imports pytest is left alone.
venv_left_alone()
{
	mkdir -p "$S/v3/bin" || return 1
	printf '#!/bin/sh\nexit 0\n' > "$S/v3/bin/python"
	chmod +x "$S/v3/bin/python"
	: > "$S/calls.log"
	make_venv "$S/v3" || return 1
	grep -qxF "$S/v3 is ready" "$S/make.log" || return 1
	calls_are ""
}
check "venv with pytest is left alone" venv_left_alone

echo "$passed/$total checks passed" >&2
[ "$passed" -eq "$total" ] || exit 1
