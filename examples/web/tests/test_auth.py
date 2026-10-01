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

"""Tests of the optional Basic authentication."""

from __future__ import annotations

import base64
import logging
import threading
import time

import pytest
from werkzeug.security import generate_password_hash

from conftest import sample_doc
from rsswitch_web import auth
from rsswitch_web.auth import (CACHE_SIZE, REALM, LoginLimiter,
                               make_auth_hook)

# A quick hash, because every test that logs in pays for it.
HASH = generate_password_hash("secret", "pbkdf2:sha256:1000")
CHALLENGE = 'Basic realm="rsswitch", charset="UTF-8"'
SWITCH = "/api/sockets/s_000000000001/switch"


def basic(user, password, scheme="Basic"):
    raw = ("%s:%s" % (user, password)).encode("utf-8")
    return {"Authorization": "%s %s" % (scheme, base64.b64encode(raw)
                                        .decode("ascii"))}


@pytest.fixture
def locked(make_app):
    """An app that wants the login admin:secret."""
    return make_app(sample_doc(), password_hash=HASH)


def is_401(resp):
    assert resp.status_code == 401
    assert resp.mimetype == "application/json"
    assert resp.headers["WWW-Authenticate"] == CHALLENGE
    error = resp.get_json()["error"]
    assert error["code"] == "unauthorized"
    assert error["message"]


def test_realm_and_cache_size():
    assert REALM == "rsswitch"
    assert CACHE_SIZE == 8


def test_no_hash_means_no_hook():
    assert make_auth_hook(None, None) is None
    assert make_auth_hook("admin", None) is None


def test_no_hash_means_open(fake_daemon, switch_app):
    fake = fake_daemon()
    client = switch_app(fake).test_client()
    assert client.get("/api/rooms").status_code == 200
    resp = client.post(SWITCH, json={"state": "on"})
    assert resp.status_code == 200
    assert fake.lines == ["0 0 0 1"]
    assert client.get("/nowhere").status_code == 404


def test_missing_credentials_is_401_with_www_authenticate(locked):
    client = locked.test_client()
    is_401(client.get("/api/rooms"))
    is_401(client.get("/api/rooms/r_000000000001"))
    is_401(client.post(SWITCH, json={"state": "on"}))


def test_401_does_not_reach_the_daemon(fake_daemon, switch_app):
    fake = fake_daemon()
    app = switch_app(fake, password_hash=HASH)
    is_401(app.test_client().post(SWITCH, json={"state": "on"}))
    assert fake.connections == 0


def test_valid_credentials_pass(locked):
    client = locked.test_client()
    resp = client.get("/api/rooms", headers=basic("admin", "secret"))
    assert resp.status_code == 200
    assert resp.get_json()["rooms"][0]["name"] == "Wohnzimmer"


def test_valid_credentials_switch(fake_daemon, switch_app):
    fake = fake_daemon()
    app = switch_app(fake, password_hash=HASH)
    resp = app.test_client().post(SWITCH, json={"state": "on"},
                                  headers=basic("admin", "secret"))
    assert resp.status_code == 200
    assert fake.lines == ["0 0 0 1"]


def test_wrong_password_and_wrong_username_are_401(locked):
    client = locked.test_client()
    is_401(client.get("/api/rooms", headers=basic("admin", "wrong")))
    is_401(client.get("/api/rooms", headers=basic("root", "secret")))
    is_401(client.get("/api/rooms", headers=basic("root", "wrong")))
    is_401(client.get("/api/rooms", headers=basic("", "")))


def test_username_is_case_sensitive(locked):
    client = locked.test_client()
    is_401(client.get("/api/rooms", headers=basic("Admin", "secret")))
    is_401(client.get("/api/rooms", headers=basic("ADMIN", "secret")))
    assert client.get("/api/rooms",
                      headers=basic("admin", "secret")).status_code == 200


def test_trailing_space_password_is_401(locked):
    client = locked.test_client()
    is_401(client.get("/api/rooms", headers=basic("admin", "secret ")))
    is_401(client.get("/api/rooms", headers=basic("admin", " secret")))
    is_401(client.get("/api/rooms", headers=basic("admin", "Secret")))


def test_default_username_admin(make_app):
    app = make_app(sample_doc(), password_hash=HASH)
    assert app.config["RSSWITCH_WEB"]["username"] == "admin"
    client = app.test_client()
    assert client.get("/api/rooms",
                      headers=basic("admin", "secret")).status_code == 200


def test_configured_username(make_app):
    app = make_app(sample_doc(), password_hash=HASH, username="Ölmühle")
    client = app.test_client()
    assert client.get("/api/rooms",
                      headers=basic("Ölmühle", "secret")).status_code == 200
    is_401(client.get("/api/rooms", headers=basic("admin", "secret")))


def b64(text):
    return base64.b64encode(text.encode("utf-8")).decode("ascii")


@pytest.mark.parametrize("header", [
    "",
    "Basic",
    "Basic ",
    "Basic !!!",
    "Basic not base64 at all",
    "Bearer x",
    "Bearer " + b64("admin:secret"),
    "Digest username=admin",
    "Basic " + b64("admin"),
    "Basic " + b64("admin:"),
    "Basic " + b64(":secret"),
    "Basic " + b64(":"),
    "Basic ä",
], ids=["empty", "scheme-only", "no-credentials", "bad-base64", "spaces",
        "bearer", "bearer-with-login", "digest", "no-colon",
        "empty-password", "empty-username", "only-colon", "non-ascii"])
def test_malformed_headers_are_401(locked, header):
    client = locked.test_client()
    is_401(client.get("/api/rooms", headers={"Authorization": header}))


def test_unknown_url_is_401_not_404(locked):
    client = locked.test_client()
    is_401(client.get("/no/such/url"))
    is_401(client.get("/api/no-such-route"))
    resp = client.get("/no/such/url", headers=basic("admin", "secret"))
    assert resp.status_code == 404


def test_wrong_method_is_401_not_405(locked):
    client = locked.test_client()
    is_401(client.delete("/api/rooms/order"))
    is_401(client.put("/api/rooms"))
    resp = client.put("/api/rooms", headers=basic("admin", "secret"),
                      json={})
    assert resp.status_code == 405


def test_bad_content_type_without_credentials_is_401_not_415(locked):
    client = locked.test_client()
    is_401(client.post(SWITCH, data='{"state": "on"}',
                       content_type="text/plain"))
    is_401(client.post(SWITCH))
    resp = client.post(SWITCH, data='{"state": "on"}',
                       content_type="text/plain",
                       headers=basic("admin", "secret"))
    assert resp.status_code == 415


def test_static_path_is_401(locked):
    client = locked.test_client()
    is_401(client.get("/static/app.js"))
    is_401(client.get("/static/"))
    is_401(client.get("/"))
    resp = client.get("/static/missing.js",
                      headers=basic("admin", "secret"))
    assert resp.status_code == 404


class Counter:
    """Wraps check_password_hash and counts the calls."""

    def __init__(self, real):
        self.real = real
        self.calls = 0

    def __call__(self, *args, **kwargs):
        self.calls += 1
        return self.real(*args, **kwargs)


@pytest.fixture
def counter(monkeypatch):
    count = Counter(auth.check_password_hash)
    monkeypatch.setattr(auth, "check_password_hash", count)
    return count


def test_accepted_header_skips_hash_check(locked, counter):
    client = locked.test_client()
    for _ in range(3):
        resp = client.get("/api/rooms", headers=basic("admin", "secret"))
        assert resp.status_code == 200
    assert counter.calls == 1


def test_wrong_credentials_are_never_cached(locked, counter):
    client = locked.test_client()
    for _ in range(3):
        is_401(client.get("/api/rooms", headers=basic("admin", "wrong")))
    assert counter.calls == 3


def test_a_wrong_password_after_a_good_one_is_still_checked(locked, counter):
    client = locked.test_client()
    assert client.get("/api/rooms",
                      headers=basic("admin", "secret")).status_code == 200
    is_401(client.get("/api/rooms", headers=basic("admin", "secret2")))
    assert counter.calls == 2


def test_hash_checked_even_for_wrong_username(locked, counter):
    is_401(locked.test_client().get("/api/rooms",
                                    headers=basic("root", "secret")))
    assert counter.calls == 1


def test_right_username_and_wrong_password_checks_the_hash(locked, counter):
    is_401(locked.test_client().get("/api/rooms",
                                    headers=basic("admin", "nope")))
    assert counter.calls == 1


def test_no_credentials_do_not_pay_for_a_hash(locked, counter):
    is_401(locked.test_client().get("/api/rooms"))
    assert counter.calls == 0


def test_cache_holds_digests_only(locked):
    hook = locked.before_request_funcs[None][0]
    headers = basic("admin", "secret")
    assert locked.test_client().get("/api/rooms",
                                    headers=headers).status_code == 200
    assert len(hook.cache) == 1
    entry = hook.cache[0]
    assert isinstance(entry, bytes) and len(entry) == 32
    raw = headers["Authorization"]
    assert raw.encode() not in hook.cache
    assert all(raw.encode() not in d and b"secret" not in d
               for d in hook.cache)


def test_auth_hook_is_the_first_before_request_hook(locked):
    hooks = locked.before_request_funcs[None]
    assert hooks[0].__name__ == "hook"
    assert hooks[1].__name__ == "content_type_gate"


def test_no_hook_is_registered_without_a_hash(make_app):
    app = make_app(sample_doc())
    names = [f.__name__ for f in app.before_request_funcs.get(None, [])]
    assert names == ["content_type_gate"]


def test_cache_is_bounded_to_8(locked):
    hook = locked.before_request_funcs[None][0]
    client = locked.test_client()
    # The scheme of a header is not case sensitive, and every spelling
    # is another header with another digest.
    schemes = ["Basic", "basic", "BASIC", "bAsic", "baSic", "basIc",
               "basiC", "BAsic", "BASic"]
    assert len(schemes) == CACHE_SIZE + 1
    for scheme in schemes:
        resp = client.get("/api/rooms",
                          headers=basic("admin", "secret", scheme))
        assert resp.status_code == 200
    assert len(hook.cache) == CACHE_SIZE
    assert len(set(hook.cache)) == CACHE_SIZE


def test_concurrent_valid_requests_all_pass(locked):
    hook = locked.before_request_funcs[None][0]
    headers = basic("admin", "secret")
    statuses = []
    start = threading.Barrier(8)

    def work():
        client = locked.test_client()
        start.wait()
        statuses.append(client.get("/api/rooms",
                                   headers=headers).status_code)

    threads = [threading.Thread(target=work) for _ in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(10)
    assert statuses == [200] * 8
    assert len(hook.cache) <= CACHE_SIZE
    assert len(hook.cache) == 1


def test_auth_never_logs_credentials(locked, caplog):
    client = locked.test_client()
    password = "s3cr3t-pa55w0rd-marker"
    header = basic("admin", password)["Authorization"]
    with caplog.at_level(logging.DEBUG):
        client.get("/api/rooms", headers=basic("admin", password))
        client.get("/api/rooms", headers=basic("admin", "secret"))
        client.get("/api/rooms", headers=basic("admin", "secret"))
    text = caplog.text
    assert password not in text
    assert header not in text
    assert HASH not in text
    assert b64("admin:" + password) not in text


class Probe:
    """Stands in for check_password_hash and counts the overlap."""

    def __init__(self, real):
        self.real = real
        self.active = 0
        self.peak = 0
        self.calls = 0
        self._lock = threading.Lock()

    def __call__(self, *args, **kwargs):
        with self._lock:
            self.calls += 1
            self.active += 1
            self.peak = max(self.peak, self.active)
        try:
            time.sleep(0.05)
            return self.real(*args, **kwargs)
        finally:
            with self._lock:
                self.active -= 1


def test_only_one_hash_check_runs_at_a_time(locked, monkeypatch):
    # Six failures from one address would lock it out.
    locked.before_request_funcs[None][0].limiter = LoginLimiter(
        max_failures=1000)
    probe = Probe(auth.check_password_hash)
    monkeypatch.setattr(auth, "check_password_hash", probe)
    statuses = []
    start = threading.Barrier(6)

    def work(n):
        client = locked.test_client()
        start.wait()
        resp = client.get("/api/rooms",
                          headers=basic("admin", "wrong-%d" % n))
        statuses.append(resp.status_code)

    threads = [threading.Thread(target=work, args=(n,)) for n in range(6)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(10)
    assert statuses == [401] * 6
    assert probe.calls == 6
    assert probe.peak == 1


def test_waiting_request_uses_a_credential_accepted_meanwhile(
        locked, monkeypatch):
    probe = Probe(auth.check_password_hash)
    monkeypatch.setattr(auth, "check_password_hash", probe)
    statuses = []
    start = threading.Barrier(4)

    def work():
        client = locked.test_client()
        start.wait()
        resp = client.get("/api/rooms", headers=basic("admin", "secret"))
        statuses.append(resp.status_code)

    threads = [threading.Thread(target=work) for _ in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(10)
    assert statuses == [200] * 4
    assert probe.calls == 1
    assert probe.peak == 1


def test_cached_credentials_do_not_wait_for_a_hash_check(locked,
                                                         monkeypatch):
    client = locked.test_client()
    assert client.get("/api/rooms",
                      headers=basic("admin", "secret")).status_code == 200
    probe = Probe(auth.check_password_hash)
    probe.real = lambda *a, **k: False
    monkeypatch.setattr(auth, "check_password_hash", probe)
    done = []

    def slow():
        locked.test_client().get("/api/rooms",
                                 headers=basic("admin", "wrong"))
        done.append(1)

    worker = threading.Thread(target=slow)
    worker.start()
    deadline = time.monotonic() + 5
    while probe.active == 0 and time.monotonic() < deadline:
        time.sleep(0.005)
    assert probe.active == 1
    resp = client.get("/api/rooms", headers=basic("admin", "secret"))
    assert resp.status_code == 200
    assert not done
    worker.join(10)
    assert done


def test_a_hash_that_cannot_be_checked_is_401_not_500(locked, monkeypatch,
                                                      caplog):
    def broken(*args, **kwargs):
        raise ValueError("memory limit exceeded")

    monkeypatch.setattr(auth, "check_password_hash", broken)
    client = locked.test_client()
    is_401(client.get("/api/rooms", headers=basic("admin", "secret")))
    assert "cannot be checked" in caplog.text


class FakeClock:
    """A clock that only moves when a test says so."""

    def __init__(self):
        self.now = 1000.0

    def __call__(self):
        return self.now

    def advance(self, seconds):
        self.now += seconds


def addr_env(addr):
    return {"REMOTE_ADDR": addr}


def fail(client, n, addr="10.0.0.1", headers=None):
    """Send n requests with a wrong password and return the statuses."""
    return [client.get("/api/rooms",
                       headers=headers or basic("admin", "wrong"),
                       environ_base=addr_env(addr)).status_code
            for _ in range(n)]


def good(client, addr="10.0.0.1", headers=None):
    return client.get("/api/rooms",
                      headers=headers or basic("admin", "secret"),
                      environ_base=addr_env(addr))


def is_429(resp, wait="60"):
    assert resp.status_code == 429
    assert resp.mimetype == "application/json"
    assert resp.headers["Retry-After"] == wait
    assert "WWW-Authenticate" not in resp.headers
    error = resp.get_json()["error"]
    assert error["code"] == "too_many_attempts"
    assert error["message"]


def test_fifth_wrong_password_locks_out_with_429(locked, counter):
    client = locked.test_client()
    assert fail(client, 5) == [401] * 5
    is_429(good(client))
    assert counter.calls == 5


def test_lockout_ends_after_the_cooldown(locked):
    hook = locked.before_request_funcs[None][0]
    clock = FakeClock()
    hook.limiter.clock = clock
    client = locked.test_client()
    fail(client, 5)
    is_429(good(client))
    clock.advance(59)
    is_429(good(client), wait="1")
    clock.advance(1)
    assert good(client).status_code == 200


def test_requests_without_credentials_never_lock(locked):
    hook = locked.before_request_funcs[None][0]
    client = locked.test_client()
    for _ in range(20):
        is_401(client.get("/api/rooms", environ_base=addr_env("10.0.0.1")))
    assert hook.limiter.retry_after("10.0.0.1") == 0
    assert len(hook.limiter) == 0
    assert good(client).status_code == 200


def test_malformed_credentials_do_not_count(locked):
    hook = locked.before_request_funcs[None][0]
    client = locked.test_client()
    for header in ("Basic !!!", "Bearer x", "Digest username=admin"):
        for _ in range(6):
            is_401(client.get("/api/rooms", headers={
                "Authorization": header}))
    assert len(hook.limiter) == 0


def test_lockout_is_per_peer_address(locked):
    client = locked.test_client()
    fail(client, 5, "10.0.0.1")
    is_429(good(client, "10.0.0.1"))
    assert good(client, "10.0.0.2").status_code == 200


def test_forwarded_for_is_ignored(locked):
    client = locked.test_client()
    fail(client, 5, "10.0.0.1")
    headers = dict(basic("admin", "secret"))
    headers["X-Forwarded-For"] = "10.0.0.9"
    is_429(good(client, "10.0.0.1", headers))
    headers["X-Forwarded-For"] = "10.0.0.1"
    assert good(client, "10.0.0.2", headers).status_code == 200
    # A spoofed header does not move the failures to another address.
    wrong = dict(basic("admin", "wrong"))
    wrong["X-Forwarded-For"] = "10.0.0.7"
    assert fail(client, 6, "10.0.0.3", wrong) == [401] * 5 + [429]
    assert good(client, "10.0.0.7").status_code == 200


def test_success_resets_the_failures(locked):
    client = locked.test_client()
    assert fail(client, 4) == [401] * 4
    assert good(client).status_code == 200
    assert fail(client, 4) == [401] * 4


def test_cached_success_resets_the_failures(locked):
    client = locked.test_client()
    assert good(client, "10.0.0.2").status_code == 200
    assert fail(client, 4) == [401] * 4
    # The header is in the cache now, and the hash is not checked.
    assert good(client).status_code == 200
    assert fail(client, 4) == [401] * 4


def test_locked_address_gets_429_on_static_and_unknown_paths(locked):
    client = locked.test_client()
    fail(client, 5)
    for path in ("/", "/static/app.js", "/no/such/url", "/api/rooms"):
        is_429(client.get(path, environ_base=addr_env("10.0.0.1")))
        is_429(client.get(path, headers=basic("admin", "secret"),
                          environ_base=addr_env("10.0.0.1")))


def test_locked_address_gets_429_even_without_credentials(locked, counter):
    client = locked.test_client()
    fail(client, 5)
    is_429(client.get("/api/rooms", environ_base=addr_env("10.0.0.1")))
    assert counter.calls == 5


def test_a_hash_that_cannot_be_checked_counts_as_a_failure(locked,
                                                           monkeypatch):
    def broken(*args, **kwargs):
        raise ValueError("memory limit exceeded")

    monkeypatch.setattr(auth, "check_password_hash", broken)
    client = locked.test_client()
    assert fail(client, 6, headers=basic("admin", "secret")) == (
        [401] * 5 + [429])


def test_lockout_logs_no_credentials(locked, caplog):
    client = locked.test_client()
    password = "s3cr3t-pa55w0rd-marker"
    header = basic("admin", password)["Authorization"]
    with caplog.at_level(logging.DEBUG):
        fail(client, 6, headers={"Authorization": header})
    text = caplog.text
    assert "10.0.0.1" in text
    assert "locked out" in text
    assert password not in text
    assert header not in text
    assert b64("admin:" + password) not in text
    assert "admin" not in text
    assert HASH not in text


def test_the_app_has_a_limiter(locked):
    hook = locked.before_request_funcs[None][0]
    assert isinstance(hook.limiter, LoginLimiter)
    assert hook.limiter.max_failures == 5
    assert hook.limiter.window == 60.0
    assert hook.limiter.lockout == 60.0
    assert hook.limiter.max_tracked == 256
