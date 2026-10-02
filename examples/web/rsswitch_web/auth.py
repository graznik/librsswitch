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

"""Optional HTTP Basic authentication for every request."""

from __future__ import annotations

import hashlib
import hmac
import logging
import math
import threading
import time

from flask import request
from werkzeug.security import check_password_hash

from .errors import ApiError

log = logging.getLogger(__name__)

REALM = "rsswitch"

# How many accepted Authorization headers are remembered.
CACHE_SIZE = 8

# How many failed logins from one address start a lockout.
MAX_FAILURES = 5

# Seconds, from the first failure of an address, in which MAX_FAILURES
# failures start a lockout. Failures further apart do not add up.
FAILURE_WINDOW = 60.0

# Seconds an address is refused after its last allowed failure.
LOCKOUT = 60.0

# How many addresses the limiter remembers at most, so that clients
# with many source addresses cannot make it grow without bound.
MAX_TRACKED = 256

_CHALLENGE = 'Basic realm="%s", charset="UTF-8"' % REALM


class LoginLimiter:
    """Count failed logins per address and lock an address out.

    max_failures failures within window seconds lock an address for
    lockout seconds. At most max_tracked addresses are remembered, in
    memory only. clock returns seconds and is an attribute, so that
    tests can replace it. One lock guards every method.
    """

    def __init__(self, max_failures=MAX_FAILURES, window=FAILURE_WINDOW,
                 lockout=LOCKOUT, max_tracked=MAX_TRACKED,
                 clock=time.monotonic):
        self.max_failures = max_failures
        self.window = window
        self.lockout = lockout
        self.max_tracked = max_tracked
        self.clock = clock
        self._lock = threading.Lock()
        # address -> [failures, first failure at, locked until or None]
        self._entries = {}

    def retry_after(self, addr) -> int:
        """Return the whole seconds addr must wait, or 0 if it may try."""
        with self._lock:
            entry = self._entries.get(addr)
            if entry is None or entry[2] is None:
                return 0
            now = self.clock()
            if now >= entry[2]:
                del self._entries[addr]
                return 0
            return max(1, math.ceil(entry[2] - now))

    def failed(self, addr) -> bool:
        """Count a failed login. Return True if it starts a lockout."""
        with self._lock:
            now = self.clock()
            self._expire(now)
            entry = self._entries.get(addr)
            if entry is None:
                if len(self._entries) >= self.max_tracked:
                    self._evict()
                entry = [0, now, None]
                self._entries[addr] = entry
            elif entry[2] is not None:
                # Already locked, nothing more to count.
                return False
            entry[0] += 1
            if entry[0] >= self.max_failures:
                entry[0] = 0
                entry[2] = now + self.lockout
                return True
            return False

    def succeeded(self, addr) -> None:
        """Forget the failures of addr after a good login."""
        with self._lock:
            self._entries.pop(addr, None)

    def __len__(self) -> int:
        with self._lock:
            return len(self._entries)

    def _expire(self, now):
        """Drop entries whose window or lock is over."""
        for addr, (_, first, until) in list(self._entries.items()):
            if until is None:
                over = now - first > self.window
            else:
                over = now >= until
            if over:
                del self._entries[addr]

    def _evict(self):
        """Make room: drop the oldest unlocked entry, else the lock
        that ends first."""
        unlocked = [(e[1], a) for a, e in self._entries.items()
                    if e[2] is None]
        if unlocked:
            del self._entries[min(unlocked)[1]]
            return
        del self._entries[min((e[2], a)
                              for a, e in self._entries.items())[1]]


def make_auth_hook(username, password_hash, limiter=None):
    """Return a before_request hook that demands the login, or None.

    Without a password_hash nothing is demanded and None is returned.
    Otherwise the hook answers 401 to every request that does not carry
    the Basic credentials of username and a password that matches
    password_hash. Register it before every other hook, so that it also
    covers unknown URLs, wrong methods and static files.

    Failed logins are counted per peer address in limiter, a
    LoginLimiter made here if none is given. A locked address gets 429
    with Retry-After, before any password is checked. The address is
    the one of the TCP connection and never a header of the request,
    which a client could set to anything. Only a login whose password
    was checked clears the failures of its address, not a header that
    was accepted before and found in the cache.

    The hook object has an attribute cache, the list of the digests of
    the accepted headers, and an attribute limiter that the tests may
    replace.
    """
    if password_hash is None:
        return None
    if limiter is None:
        limiter = LoginLimiter()

    # The default hash of Werkzeug is scrypt. It costs about 57 ms and
    # 32 MiB per check on x86_64 and plausibly seconds on a slow board
    # such as an ARMv6 Raspberry Pi, and Basic authentication sends the
    # credentials again with every request. So a header that was
    # accepted once is remembered, as its SHA-256 digest and never as
    # the header itself, and the hash is checked only once per
    # credential. Wrong credentials are never remembered and always pay
    # the full cost.
    #
    # That cost is also what anybody who can reach the app can make it
    # pay, with a new wrong password in every request. So at most one
    # check runs at a time, which bounds the memory and the CPU that
    # logins can take from the daemon client threads. A request waits
    # its turn, and looks at the cache again when it gets it, because
    # the credential it carries may have been accepted in the meantime.
    cache = []
    lock = threading.Lock()
    verify_lock = threading.Lock()
    wanted_user = username.encode("utf-8")

    def locked_out(addr):
        wait = hook.limiter.retry_after(addr)
        if wait:
            raise ApiError(429, "too_many_attempts",
                           "too many failed logins, try again later",
                           headers={"Retry-After": str(wait)})

    def hook():
        addr = request.remote_addr or ""
        locked_out(addr)
        header = request.headers.get("Authorization", "")
        try:
            auth = request.authorization
        except ValueError:
            # Werkzeug does not catch a header with non-ASCII text.
            auth = None
        if (auth is not None and auth.type == "basic"
                and auth.username is not None
                and auth.password is not None):
            digest = hashlib.sha256(header.encode("utf-8")).digest()
            with lock:
                if any(hmac.compare_digest(digest, d) for d in cache):
                    # No succeeded() here: the hash was not checked
                    # now, and a client that shares the address with
                    # the owner could otherwise have its failures
                    # cleared by every request of the owner.
                    return None
            # Both checks always run, so that the time does not tell
            # which of the two was wrong.
            user_ok = hmac.compare_digest(auth.username.encode("utf-8"),
                                          wanted_user)
            with verify_lock:
                with lock:
                    if any(hmac.compare_digest(digest, d) for d in cache):
                        return None
                # Requests that waited here may belong to an address
                # that was locked while they waited.
                locked_out(addr)
                try:
                    pass_ok = check_password_hash(password_hash,
                                                  auth.password)
                except ValueError as e:
                    # A hash that cannot be checked refuses the login
                    # like a wrong password, and does not become a 500.
                    log.error("password_hash cannot be checked: %s", e)
                    pass_ok = False
            if user_ok and pass_ok:
                with lock:
                    if not any(hmac.compare_digest(digest, d)
                               for d in cache):
                        cache.append(digest)
                        del cache[:-CACHE_SIZE]
                hook.limiter.succeeded(addr)
                return None
            if hook.limiter.failed(addr):
                log.warning("too many failed logins from %s, locked out "
                            "for %d s", addr, hook.limiter.lockout)
        raise ApiError(401, "unauthorized", "authentication required",
                       headers={"WWW-Authenticate": _CHALLENGE})

    hook.cache = cache
    hook.limiter = limiter
    return hook
