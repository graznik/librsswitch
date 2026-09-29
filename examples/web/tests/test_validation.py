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
from rsswitch_web.validation import (check_socket_fields, clean_name,
                                     name_key)

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
    assert PRECOMPOSED != COMBINING
    assert (name_key(PRECOMPOSED) == name_key(COMBINING)
            == name_key("KÜCHE") == name_key("küche"))
    assert name_key("Kueche") != name_key(PRECOMPOSED)


def test_name_key_folds_what_lower_does_not():
    assert name_key("Straße") == name_key("STRASSE")


def test_name_key_is_normalised_after_folding():
    key = name_key("ẛ̣")
    assert key == name_key(key)


def fields(type_name, group, number):
    return {"type": type_name, "group": group, "number": number}


def problem_of(given):
    with pytest.raises(ApiError) as info:
        check_socket_fields(given)
    assert info.value.status == 422
    assert info.value.code == "validation"
    return info.value.extra["field"]


@pytest.mark.parametrize("type_name, group, number", [
    ("rev_008345", 0, 0),
    ("rev_008345", 3, 2),
    ("rev_008345", 0, 2),
    ("rev_008345", 3, 0),
    ("pollin_2605", 0, 0),
    ("pollin_2605", 31, 31),
    ("pollin_2605", 4, 3),
    ("pollin_2605", 17, 23),
])
def test_check_socket_fields_accepts(type_name, group, number):
    given = fields(type_name, group, number)
    assert check_socket_fields(given) == given


def test_check_socket_fields_returns_only_the_address():
    given = fields("rev_008345", 1, 1)
    given["name"] = "Lamp"
    assert check_socket_fields(given) == fields("rev_008345", 1, 1)


@pytest.mark.parametrize("type_name, group, number, field", [
    ("rev_008345", 4, 0, "group"),
    ("rev_008345", 0, 3, "number"),
    ("rev_008345", -1, 0, "group"),
    ("rev_008345", 0, -1, "number"),
    ("pollin_2605", 32, 0, "group"),
    ("pollin_2605", 0, 32, "number"),
    ("pollin_2605", -1, 0, "group"),
])
def test_check_socket_fields_refuses_out_of_range(type_name, group, number,
                                                  field):
    assert problem_of(fields(type_name, group, number)) == field


@pytest.mark.parametrize("value", [
    True, False, 1.0, 3.0, "3", None, 2 ** 64, -2 ** 64, [1], {"a": 1},
])
@pytest.mark.parametrize("field", ["group", "number"])
def test_check_socket_fields_refuses_what_is_not_a_small_integer(field,
                                                                 value):
    given = fields("pollin_2605", 1, 1)
    given[field] = value
    assert problem_of(given) == field


@pytest.mark.parametrize("value", [
    "REV_008345", " rev_008345", "rev_008345 ", "rev", "", None, 5, True,
    ["rev_008345"],
])
def test_check_socket_fields_refuses_a_bad_type(value):
    assert problem_of(fields(value, 0, 0)) == "type"


def test_check_socket_fields_reports_the_type_before_the_group():
    assert problem_of({"group": 99, "number": 99}) == "type"
    assert problem_of(fields("nope", 99, 99)) == "type"


def test_check_socket_fields_reports_the_group_before_the_number():
    assert problem_of(fields("rev_008345", 9, 9)) == "group"
    assert problem_of({"type": "rev_008345"}) == "group"
    assert problem_of({"type": "rev_008345", "group": 0}) == "number"


def test_check_socket_fields_does_not_mix_the_limits_of_the_types():
    assert check_socket_fields(fields("pollin_2605", 4, 3))
    assert problem_of(fields("rev_008345", 4, 0)) == "group"
    assert problem_of(fields("rev_008345", 0, 3)) == "number"


def test_check_socket_fields_names_the_limit():
    with pytest.raises(ApiError) as info:
        check_socket_fields(fields("rev_008345", 4, 0))
    assert info.value.message == "must be an integer 0-3"
    with pytest.raises(ApiError) as info:
        check_socket_fields(fields("nope", 0, 0))
    assert info.value.message == "must be one of rev_008345, pollin_2605"
