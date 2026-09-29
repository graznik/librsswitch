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

"""Smoke tests of the client against the real x86_64 rsswitchd.

The fake daemon of the other tests is written from the protocol
description. These tests run the client against the program itself, so
the two cannot drift apart unnoticed. They are skipped when
examples/network/rsswitchd is missing or not an x86_64 build.
"""

from __future__ import annotations

import pytest

from rsswitch_web.daemon import DaemonReplyError, RsswitchdClient
from rsswitch_web.errors import ApiError
from rsswitch_web.model import SOCKET_TYPES
from rsswitch_web.validation import check_socket_fields


def client_for(real_daemon):
    return RsswitchdClient("127.0.0.1", real_daemon.port, 2, 5)


def test_real_daemon_pollin_17_23_on_prints_codeword(real_daemon):
    client_for(real_daemon).send(1, 17, 23, 1)
    assert real_daemon.wait_for_stdout("0FFF0000F00F", 2)


def test_real_daemon_rev_3_2_on_is_ok(real_daemon):
    client_for(real_daemon).send(0, 3, 2, 1)


def test_real_daemon_group_40_is_err_range(real_daemon):
    with pytest.raises(DaemonReplyError) as info:
        client_for(real_daemon).send(1, 40, 0, 1)
    assert info.value.daemon_class == "range"
    assert info.value.daemon_message == "group 40 not in 0-31"


def test_real_daemon_serves_consecutive_connections(real_daemon):
    client = client_for(real_daemon)
    client.send(1, 17, 23, 1)
    client.send(1, 17, 23, 0)


def refused_field(type_name, group, number):
    """Return the field check_socket_fields() names, or None."""
    try:
        check_socket_fields({"type": type_name, "group": group,
                             "number": number})
    except ApiError as e:
        return e.extra["field"]
    return None


@pytest.mark.parametrize("type_name", sorted(SOCKET_TYPES))
def test_limits_match_the_real_daemon(real_daemon, type_name):
    """The address limits of the web app are those of rsswitchd.

    The largest group and number are accepted by both. One more is
    refused by both, and each refusal names the same field.
    """
    limits = SOCKET_TYPES[type_name]
    client = client_for(real_daemon)
    top_group = limits.groups - 1
    top_number = limits.sockets - 1

    assert refused_field(type_name, top_group, top_number) is None
    client.send(limits.dev, top_group, top_number, 1)

    assert refused_field(type_name, limits.groups, 0) == "group"
    with pytest.raises(DaemonReplyError) as info:
        client.send(limits.dev, limits.groups, 0, 1)
    assert info.value.daemon_class == "range"
    assert info.value.daemon_message.startswith("group ")

    assert refused_field(type_name, 0, limits.sockets) == "number"
    with pytest.raises(DaemonReplyError) as info:
        client.send(limits.dev, 0, limits.sockets, 1)
    assert info.value.daemon_class == "range"
    assert info.value.daemon_message.startswith("socket ")
