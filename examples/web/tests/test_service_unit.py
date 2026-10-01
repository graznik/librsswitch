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

"""Tests of rsswitch-web.service: what it runs, as whom, and how it is
sandboxed."""

from __future__ import annotations

import os

import pytest

from rsswitch_web.config import DEFAULT_CONFIG_PATH, DEFAULTS

from conftest import WEB_DIR

UNIT = WEB_DIR / "rsswitch-web.service"

EXEC_START = ("/opt/rsswitch-web/venv/bin/python -m rsswitch_web "
              "--config /etc/rsswitch-web/config.json serve")

SANDBOX = {
    "NoNewPrivileges": "yes",
    "ProtectSystem": "strict",
    "ProtectHome": "yes",
    "PrivateTmp": "yes",
    "PrivateDevices": "yes",
    "ProtectKernelTunables": "yes",
    "ProtectKernelModules": "yes",
    "ProtectControlGroups": "yes",
    "LockPersonality": "yes",
    "RestrictAddressFamilies": "AF_INET AF_INET6 AF_UNIX",
    "UMask": "0077",
}


def parse():
    """Return {section: [(key, value, commented)]} of the unit.

    commented tells whether the line before the directive is a comment.
    """
    sections = {}
    current = None
    previous = ""
    for line in UNIT.read_text().splitlines():
        text = line.strip()
        if text.startswith("[") and text.endswith("]"):
            current = sections.setdefault(text[1:-1], [])
        elif text and not text.startswith("#"):
            assert current is not None, "directive outside a section"
            key, sep, value = text.partition("=")
            assert sep, "not a directive: %r" % line
            current.append((key, value, previous.startswith("#")))
        previous = text
    return sections


def values(section, key):
    return [v for k, v, _ in parse()[section] if k == key]


def one(section, key):
    found = values(section, key)
    assert len(found) == 1, "%s= appears %d times" % (key, len(found))
    return found[0]


def test_unit_runs_serve_as_rsswitch_web():
    assert one("Service", "Type") == "simple"
    assert one("Service", "User") == "rsswitch-web"
    assert one("Service", "WorkingDirectory") == "/opt/rsswitch-web"
    assert one("Service", "ExecStart") == EXEC_START


def test_unit_paths_match_the_app_defaults():
    words = one("Service", "ExecStart").split()
    assert words[words.index("--config") + 1] == DEFAULT_CONFIG_PATH
    assert "/var/lib/" + one("Service", "StateDirectory") \
        == os.path.dirname(DEFAULTS["data_file"])
    assert one("Service", "StateDirectoryMode") == "0750"


@pytest.mark.parametrize("key,value", sorted(SANDBOX.items()))
def test_unit_is_sandboxed(key, value):
    assert one("Service", key) == value


def test_unit_follows_rsswitchd():
    assert one("Unit", "After").split() == ["network.target",
                                            "rsswitchd.service"]
    assert one("Unit", "Wants") == "rsswitchd.service"
    assert one("Service", "Restart") == "on-failure"
    assert one("Service", "RestartSec") == "2"
    assert one("Install", "WantedBy") == "multi-user.target"


def test_unit_grants_nothing_extra():
    keys = [k for k, _, _ in parse()["Service"]]
    for key in ("Nice", "SupplementaryGroups", "Group", "AmbientCapabilities",
                "CapabilityBoundingSet", "DynamicUser"):
        assert key not in keys
    assert values("Service", "User") == ["rsswitch-web"]


def test_every_service_directive_has_a_comment():
    bare = [k for k, _, commented in parse()["Service"]
            if not commented and k != "Type"]
    assert bare == []


def test_unit_has_only_known_sections():
    assert list(parse()) == ["Unit", "Service", "Install"]
