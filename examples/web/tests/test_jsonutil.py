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

"""Tests of the strict JSON parser."""

from __future__ import annotations

import pytest

from rsswitch_web.jsonutil import strict_loads


@pytest.mark.parametrize("raw", [
    "NaN", "Infinity", "-Infinity",
    '{"a": NaN}', '[1, Infinity]', '{"a": {"b": -Infinity}}',
    b"NaN", b'{"a": Infinity}',
], ids=["nan", "inf", "minus-inf", "nan-in-object", "inf-in-list",
        "nested", "nan-bytes", "inf-bytes"])
def test_nan_and_infinity_are_refused(raw):
    with pytest.raises(ValueError, match="invalid JSON constant"):
        strict_loads(raw)


@pytest.mark.parametrize("raw", [b"\xff", b'{"a": "\xc3"}', b"\xef\xbb"])
def test_invalid_utf8_is_a_value_error(raw):
    with pytest.raises(ValueError):
        strict_loads(raw)


@pytest.mark.parametrize("raw", ["[" * 100000, b"[" * 100000,
                                 '{"a":' * 100000])
def test_deep_nesting_is_a_value_error(raw):
    with pytest.raises(ValueError, match="nested too deeply"):
        strict_loads(raw)


@pytest.mark.parametrize("raw", ["", "{", "[1,]", b"nope", "{'a': 1}"])
def test_bad_json_is_a_value_error(raw):
    with pytest.raises(ValueError):
        strict_loads(raw)


def test_good_documents_are_parsed():
    assert strict_loads(b'{"a": 1}') == {"a": 1}
    assert strict_loads('{"a": 1}') == {"a": 1}
    assert strict_loads(bytearray(b"[1, 2]")) == [1, 2]
    assert strict_loads('"K\\u00fcche"') == "K\u00fcche"
    assert strict_loads('"K\u00fcche"'.encode("utf-8")) == "K\u00fcche"
