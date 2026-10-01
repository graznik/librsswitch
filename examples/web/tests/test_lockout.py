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

"""Tests of the failed-login limiter, with a clock that is set by hand."""

from __future__ import annotations

import threading

from rsswitch_web.auth import (FAILURE_WINDOW, LOCKOUT, MAX_FAILURES,
                               MAX_TRACKED, LoginLimiter)

A = "10.0.0.1"
B = "10.0.0.2"


class FakeClock:
    """A clock that only moves when a test says so."""

    def __init__(self):
        self.now = 1000.0

    def __call__(self):
        return self.now

    def advance(self, seconds):
        self.now += seconds


def make(**kwargs):
    clock = FakeClock()
    return LoginLimiter(clock=clock, **kwargs), clock


def test_constants():
    assert MAX_FAILURES == 5
    assert FAILURE_WINDOW == 60.0
    assert LOCKOUT == 60.0
    assert MAX_TRACKED == 256


def test_four_failures_do_not_lock():
    limiter, _ = make()
    assert [limiter.failed(A) for _ in range(4)] == [False] * 4
    assert limiter.retry_after(A) == 0


def test_fifth_failure_locks_for_60_seconds():
    limiter, clock = make()
    assert [limiter.failed(A) for _ in range(5)] == [False] * 4 + [True]
    assert limiter.retry_after(A) == 60
    clock.advance(59.5)
    assert limiter.retry_after(A) == 1
    clock.advance(0.5)
    assert limiter.retry_after(A) == 0
    assert len(limiter) == 0


def test_the_count_starts_again_after_a_lock():
    limiter, clock = make()
    for _ in range(5):
        limiter.failed(A)
    clock.advance(60)
    assert limiter.retry_after(A) == 0
    assert [limiter.failed(A) for _ in range(4)] == [False] * 4
    assert limiter.failed(A) is True


def test_failures_outside_the_window_do_not_add_up():
    limiter, clock = make()
    for _ in range(4):
        limiter.failed(A)
    clock.advance(61)
    assert [limiter.failed(A) for _ in range(4)] == [False] * 4
    assert limiter.retry_after(A) == 0


def test_the_window_starts_at_the_first_failure():
    limiter, clock = make()
    limiter.failed(A)
    clock.advance(59)
    for _ in range(3):
        limiter.failed(A)
    clock.advance(1)
    assert limiter.failed(A) is True


def test_success_clears_the_count():
    limiter, _ = make()
    for _ in range(4):
        limiter.failed(A)
    limiter.succeeded(A)
    assert len(limiter) == 0
    assert [limiter.failed(A) for _ in range(4)] == [False] * 4
    assert limiter.retry_after(A) == 0


def test_success_of_an_unknown_address_is_fine():
    limiter, _ = make()
    limiter.succeeded(A)
    assert len(limiter) == 0


def test_addresses_are_independent():
    limiter, _ = make()
    for _ in range(5):
        limiter.failed(A)
    assert limiter.retry_after(A) == 60
    assert limiter.retry_after(B) == 0
    assert limiter.failed(B) is False


def test_a_locked_address_does_not_extend_its_lock():
    limiter, clock = make()
    for _ in range(5):
        limiter.failed(A)
    clock.advance(30)
    assert limiter.failed(A) is False
    assert limiter.retry_after(A) == 30


def test_table_is_bounded():
    limiter, _ = make()
    for n in range(MAX_TRACKED + 50):
        limiter.failed("10.1.%d.%d" % (n // 256, n % 256))
    assert len(limiter) <= MAX_TRACKED


def test_expired_entries_are_dropped_first():
    limiter, clock = make(max_tracked=4)
    for n in range(4):
        limiter.failed("old-%d" % n)
    clock.advance(61)
    limiter.failed("new")
    assert len(limiter) == 1


def test_the_oldest_unlocked_entry_goes_first():
    limiter, clock = make(max_tracked=3)
    for name in ("a", "b", "c"):
        limiter.failed(name)
        clock.advance(1)
    limiter.failed("d")
    # a was the oldest, so b still has its failure and c as well.
    for name in ("b", "c"):
        for _ in range(3):
            limiter.failed(name)
        assert limiter.failed(name) is True
    assert limiter.failed("a") is False


def test_full_table_keeps_locked_entries():
    limiter, _ = make()
    for _ in range(5):
        limiter.failed(A)
    for n in range(MAX_TRACKED):
        limiter.failed("10.2.%d.%d" % (n // 256, n % 256))
    assert len(limiter) <= MAX_TRACKED
    assert limiter.retry_after(A) == 60


def test_a_table_of_locks_drops_the_one_that_ends_first():
    limiter, clock = make(max_tracked=2, max_failures=1)
    limiter.failed("a")
    clock.advance(1)
    limiter.failed("b")
    clock.advance(1)
    limiter.failed("c")
    assert len(limiter) == 2
    assert limiter.retry_after("a") == 0
    assert limiter.retry_after("b") > 0
    assert limiter.retry_after("c") > 0


def test_concurrent_failures_are_counted_exactly():
    limiter, _ = make(max_failures=25)
    results = {}
    start = threading.Barrier(8)

    def work(n):
        addr = "10.3.0.%d" % n
        start.wait()
        results[n] = [limiter.failed(addr) for _ in range(25)]

    threads = [threading.Thread(target=work, args=(n,)) for n in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(10)
    for n in range(8):
        assert results[n] == [False] * 24 + [True]
        assert limiter.retry_after("10.3.0.%d" % n) > 0


def test_concurrent_failures_of_one_address_are_not_lost():
    limiter, _ = make(max_failures=200)
    locks = []
    start = threading.Barrier(8)

    def work():
        start.wait()
        locks.extend(limiter.failed(A) for _ in range(25))

    threads = [threading.Thread(target=work) for _ in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(10)
    assert locks.count(True) == 1
    assert limiter.retry_after(A) == 60
