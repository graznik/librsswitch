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

"""Tests of the real_daemon fixture: which binary it starts, and when a
missing one fails the test instead of skipping it."""

from __future__ import annotations

import os
import platform
import subprocess
import sys

import pytest

from conftest import WEB_DIR

TARGET = "tests/test_real_daemon.py::test_real_daemon_rev_3_2_on_is_ok"
NETWORK_BIN = WEB_DIR.parent / "network" / "rsswitchd"


def run_target(**env):
    """Run the target test in a pytest of its own and return the result.

    The environment is that of this run without RSSWITCHD_BIN and
    RSSWITCH_REQUIRE_DAEMON, plus env.
    """
    environ = {k: v for k, v in os.environ.items()
               if k not in ("RSSWITCHD_BIN", "RSSWITCH_REQUIRE_DAEMON")}
    environ.update(env)
    return subprocess.run(
        [sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider",
         TARGET],
        cwd=str(WEB_DIR), env=environ, stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        timeout=60)


def test_missing_daemon_fails_when_required(tmp_path):
    result = run_target(RSSWITCHD_BIN=str(tmp_path / "missing"),
                        RSSWITCH_REQUIRE_DAEMON="1")
    assert result.returncode == 1, result.stdout
    # A failure in a fixture is counted as an error, not as a failed test.
    assert "1 error" in result.stdout
    assert "1 skipped" not in result.stdout


def test_missing_daemon_is_skipped_otherwise(tmp_path):
    result = run_target(RSSWITCHD_BIN=str(tmp_path / "missing"))
    assert result.returncode == 0, result.stdout
    assert "1 skipped" in result.stdout


def test_named_daemon_is_used():
    named = os.environ.get("RSSWITCHD_BIN")
    if named:
        env = {"RSSWITCHD_BIN": named}
    elif platform.machine() == "x86_64" and NETWORK_BIN.is_file():
        env = {"RSSWITCHD_BIN": str(NETWORK_BIN)}
    elif os.environ.get("RSSWITCH_REQUIRE_DAEMON") == "1":
        pytest.fail("no rsswitchd to run: set RSSWITCHD_BIN")
    else:
        pytest.skip("no rsswitchd to run: set RSSWITCHD_BIN")
    result = run_target(**env)
    assert result.returncode == 0, result.stdout
    assert "1 passed" in result.stdout
