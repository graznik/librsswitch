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
import threading

from flask import request
from werkzeug.security import check_password_hash

from .errors import ApiError

log = logging.getLogger(__name__)

REALM = "rsswitch"

# How many accepted Authorization headers are remembered.
CACHE_SIZE = 8

_CHALLENGE = 'Basic realm="%s", charset="UTF-8"' % REALM


def make_auth_hook(username, password_hash):
    """Return a before_request hook that demands the login, or None.

    Without a password_hash nothing is demanded and None is returned.
    Otherwise the hook answers 401 to every request that does not carry
    the Basic credentials of username and a password that matches
    password_hash. Register it before every other hook, so that it also
    covers unknown URLs, wrong methods and static files.

    The hook object has an attribute cache, the list of the digests of
    the accepted headers, for the tests.
    """
    if password_hash is None:
        return None

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

    def hook():
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
                    return None
            # Both checks always run, so that the time does not tell
            # which of the two was wrong.
            user_ok = hmac.compare_digest(auth.username.encode("utf-8"),
                                          wanted_user)
            with verify_lock:
                with lock:
                    if any(hmac.compare_digest(digest, d) for d in cache):
                        return None
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
                return None
        raise ApiError(401, "unauthorized", "authentication required",
                       headers={"WWW-Authenticate": _CHALLENGE})

    hook.cache = cache
    return hook
