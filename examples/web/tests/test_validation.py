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

"""Tests of the pure checks in rsswitch_web.validation."""

from __future__ import annotations

import pytest

from rsswitch_web.errors import ApiError
from rsswitch_web.validation import clean_name, name_key

PRECOMPOSED = "Küche"
COMBINING = "Küche"


@pytest.mark.parametrize("value, expected", [
    ("Wohnzimmer", "Wohnzimmer"),
    ("  Küche ", PRECOMPOSED),
    (COMBINING, PRECOMPOSED),
    ("ä" * 64, "ä" * 64),
    ("a" * 64, "a" * 64),
    ("Zimmer 1", "Zimmer 1"),
    ("a b", "a b"),
])
def test_clean_name_accepts(value, expected):
    assert clean_name(value) == expected


def test_clean_name_counts_code_points_after_normalisation():
    # 64 combining forms are 128 code points before NFC and 64 after.
    assert clean_name("ü" * 64) == "ü" * 64


@pytest.mark.parametrize("value, message", [
    ("", "must not be empty"),
    ("   ", "must not be empty"),
    ("\n", "must not be empty"),
    ("a" * 65, "must be at most 64 characters"),
    ("ä" * 65, "must be at most 64 characters"),
    ("a\nb", "must not contain control characters"),
    ("a\tb", "must not contain control characters"),
    ("\x00", "must not contain control characters"),
    ("a\x7fb", "must not contain control characters"),
    ("a\ud800b", "must not contain invalid characters"),
    (5, "must be a string"),
    (None, "must be a string"),
    (["x"], "must be a string"),
    (True, "must be a string"),
    (b"abc", "must be a string"),
])
def test_clean_name_refuses(value, message):
    with pytest.raises(ApiError) as info:
        clean_name(value)
    assert info.value.status == 422
    assert info.value.code == "validation"
    assert info.value.message == message
    assert info.value.extra == {"field": "name"}


def test_clean_name_reports_the_field_it_is_given():
    with pytest.raises(ApiError) as info:
        clean_name("", field="label")
    assert info.value.extra == {"field": "label"}


def test_name_key():
    assert (name_key(PRECOMPOSED) == name_key(COMBINING)
            == name_key("KÜCHE") == name_key("küche"))
    assert name_key("Kueche") != name_key(PRECOMPOSED)


def test_name_key_folds_what_lower_does_not():
    assert name_key("Straße") == name_key("STRASSE")


def test_name_key_is_normalised_after_folding():
    key = name_key("ẛ̣")
    assert key == name_key(key)
