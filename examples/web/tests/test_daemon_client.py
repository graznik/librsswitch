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

"""Tests of the rsswitchd client against a fake daemon over TCP."""

from __future__ import annotations

import socket
import time

import pytest
from conftest import CLOSE, HANG, RESET, Drip, refused_port

from rsswitch_web import daemon
from rsswitch_web.daemon import (MAX_MESSAGE, DaemonError, DaemonProtocolError,
                                 DaemonReplyError, DaemonTimeout,
                                 DaemonUnreachable, RsswitchdClient)


# Seconds a test allows on top of the timeout it checks. A fixed budget
# below a second fails on a loaded machine or a slow board although the
# code is right, and a missing timeout still overshoots this by far:
# the fake daemon hangs, or drips for ten seconds.
SLACK = 1.0


def client_for(fake, connect_timeout=1, read_timeout=1):
    return RsswitchdClient("127.0.0.1", fake.port, connect_timeout,
                           read_timeout)


def test_ok_sends_exact_v2_line(fake_daemon):
    fake = fake_daemon()
    client_for(fake).send(1, 17, 23, 1)
    assert fake.lines == ["1 17 23 1"]
    assert fake.connections == 1


def test_line_has_no_leading_zeros(fake_daemon):
    fake = fake_daemon()
    client_for(fake).send(0, 3, 2, 0)
    assert fake.lines == ["0 3 2 0"]


@pytest.mark.parametrize("bad", [True, "1", 1.0, -1])
@pytest.mark.parametrize("position", [0, 1, 2, 3])
def test_bad_argument_raises_before_connecting(fake_daemon, bad, position):
    fake = fake_daemon()
    args = [1, 17, 23, 1]
    args[position] = bad
    with pytest.raises(ValueError):
        client_for(fake).send(*args)
    assert fake.connections == 0


def test_data_other_than_0_and_1_raises_before_connecting(fake_daemon):
    fake = fake_daemon()
    with pytest.raises(ValueError):
        client_for(fake).send(1, 17, 23, 2)
    assert fake.connections == 0


def test_err_reply_passes_class_and_text(fake_daemon):
    fake = fake_daemon(lambda line: b"ERR range group 40 not in 0-31\n")
    with pytest.raises(DaemonReplyError) as info:
        client_for(fake).send(1, 40, 0, 1)
    assert info.value.daemon_class == "range"
    assert info.value.daemon_message == "group 40 not in 0-31"
    assert info.value.code == "daemon_error"
    assert info.value.status == 502


def test_err_without_text(fake_daemon):
    fake = fake_daemon(lambda line: b"ERR send\n")
    with pytest.raises(DaemonReplyError) as info:
        client_for(fake).send(1, 1, 1, 1)
    assert info.value.daemon_class == "send"
    assert info.value.daemon_message == ""


def test_unknown_lowercase_err_class_passes_through(fake_daemon):
    fake = fake_daemon(lambda line: b"ERR future x\n")
    with pytest.raises(DaemonReplyError) as info:
        client_for(fake).send(1, 1, 1, 1)
    assert info.value.daemon_class == "future"
    assert info.value.daemon_message == "x"


def test_crlf_ok_is_ok(fake_daemon):
    fake = fake_daemon(lambda line: b"OK\r\n")
    client_for(fake).send(1, 1, 1, 1)


@pytest.mark.parametrize("reply", [b"WAT\n", b"ok\n", b"OK extra\n",
                                   b"ERR\n", b"ERR Range x\n"])
def test_garbage_is_protocol_error(fake_daemon, reply):
    fake = fake_daemon(lambda line: reply)
    with pytest.raises(DaemonProtocolError) as info:
        client_for(fake).send(1, 1, 1, 1)
    assert info.value.code == "daemon_protocol"
    assert info.value.status == 502


def test_eof_before_reply_is_protocol_error(fake_daemon):
    fake = fake_daemon(lambda line: CLOSE)
    with pytest.raises(DaemonProtocolError):
        client_for(fake).send(1, 1, 1, 1)


def test_reset_before_reply_is_protocol_error(fake_daemon):
    fake = fake_daemon(lambda line: RESET)
    with pytest.raises(DaemonProtocolError):
        client_for(fake).send(1, 1, 1, 1)


def test_overlong_reply_is_protocol_error(fake_daemon):
    fake = fake_daemon(lambda line: b"x" * 300)
    with pytest.raises(DaemonProtocolError):
        client_for(fake).send(1, 1, 1, 1)


def test_refused_is_unreachable_at_once(monkeypatch):
    attempts = []
    real = socket.create_connection

    def counting(*args, **kwargs):
        attempts.append(args)
        return real(*args, **kwargs)

    monkeypatch.setattr(daemon.socket, "create_connection", counting)
    connect_timeout = 1
    client = RsswitchdClient("127.0.0.1", refused_port(), connect_timeout, 1)
    start = time.monotonic()
    with pytest.raises(DaemonUnreachable) as info:
        client.send(1, 1, 1, 1)
    assert time.monotonic() - start < connect_timeout + SLACK
    assert len(attempts) == 1
    assert info.value.code == "daemon_unreachable"
    assert info.value.status == 502


def test_connect_timeout_is_daemon_timeout(monkeypatch):
    def timing_out(*args, **kwargs):
        raise socket.timeout("timed out")

    monkeypatch.setattr(daemon.socket, "create_connection", timing_out)
    with pytest.raises(DaemonTimeout) as info:
        RsswitchdClient("127.0.0.1", 1, 1, 1).send(1, 1, 1, 1)
    assert info.value.code == "daemon_timeout"
    assert info.value.status == 504


def test_hang_times_out_within_budget(fake_daemon):
    fake = fake_daemon(lambda line: HANG)
    read_timeout = 0.3
    start = time.monotonic()
    with pytest.raises(DaemonTimeout):
        client_for(fake, read_timeout=read_timeout).send(1, 1, 1, 1)
    assert time.monotonic() - start < read_timeout + SLACK


def test_drip_is_cut_by_total_deadline(fake_daemon):
    fake = fake_daemon(lambda line: Drip(b"x" * 100, 0.1))
    read_timeout = 0.35
    start = time.monotonic()
    with pytest.raises(DaemonTimeout):
        client_for(fake, read_timeout=read_timeout).send(1, 1, 1, 1)
    assert time.monotonic() - start < read_timeout + SLACK


def test_each_command_uses_its_own_connection(fake_daemon):
    fake = fake_daemon()
    client = client_for(fake)
    for _ in range(3):
        client.send(1, 1, 1, 1)
    assert fake.connections == 3
    assert len(fake.lines) == 3


def test_reply_text_is_sanitised(fake_daemon):
    text = b"a\x1b[31m" + b"b" * 200
    fake = fake_daemon(lambda line: b"ERR range " + text + b"\n")
    with pytest.raises(DaemonReplyError) as info:
        client_for(fake).send(1, 1, 1, 1)
    message = info.value.daemon_message
    assert "\x1b" not in message
    assert len(message) <= MAX_MESSAGE
    assert all(" " <= c <= "~" for c in message)


def test_every_failure_is_a_daemon_error(fake_daemon):
    fake = fake_daemon(lambda line: b"WAT\n")
    with pytest.raises(DaemonError):
        client_for(fake).send(1, 1, 1, 1)
