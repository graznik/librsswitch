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

"""Static checks of the page and its assets.

The project has no browser tests and no Node. These checks stand in for
them: what the server sends, what the page loads, and the rules the
script must keep, such as showing names as text only.
"""

from __future__ import annotations

import base64
import re
from html.parser import HTMLParser
from pathlib import Path

import pytest
from werkzeug.security import generate_password_hash

import rsswitch_web
from conftest import sample_doc
from rsswitch_web.app import SECURITY_HEADERS

STATIC = Path(rsswitch_web.__file__).parent / "static"

# A quick hash, because the test that logs in pays for it.
HASH = generate_password_hash("secret", "pbkdf2:sha256:1000")


def basic(user, password):
    raw = ("%s:%s" % (user, password)).encode("utf-8")
    return {"Authorization": "Basic " + base64.b64encode(raw)
            .decode("ascii")}


def source(name):
    return (STATIC / name).read_text(encoding="utf-8")


class Elements(HTMLParser):
    """Collect the start tags of a page as (tag, attributes) pairs."""

    def __init__(self):
        super().__init__()
        self.tags = []

    def handle_starttag(self, tag, attrs):
        self.tags.append((tag, dict(attrs)))


def parse(name="index.html"):
    parser = Elements()
    parser.feed(source(name))
    return parser.tags


@pytest.fixture
def client(make_app):
    return make_app(sample_doc()).test_client()


def test_index_is_served_as_html(client):
    resp = client.get("/")
    assert resp.status_code == 200
    assert resp.mimetype == "text/html"
    body = resp.get_data(as_text=True)
    assert '<html lang="en">' in body
    tags = parse()
    metas = [a for t, a in tags if t == "meta"]
    assert {"name": "viewport",
            "content": "width=device-width, initial-scale=1"} in metas
    scripts = [a for t, a in tags if t == "script"]
    assert scripts == [{"src": "/static/app.js", "defer": None}]
    links = [a for t, a in tags if t == "link"]
    assert links == [{"rel": "stylesheet", "href": "/static/app.css"}]


def test_assets_have_their_types(client):
    js = client.get("/static/app.js")
    assert js.status_code == 200
    assert js.mimetype in ("text/javascript", "application/javascript")
    css = client.get("/static/app.css")
    assert css.status_code == 200
    assert css.mimetype == "text/css"


def test_assets_are_local_only():
    for tag, attrs in parse():
        for name in ("src", "href", "action", "poster", "srcset"):
            if name in attrs:
                assert attrs[name].startswith("/static/"), (tag, attrs)
    for name in ("index.html", "app.js", "app.css"):
        text = source(name)
        assert "http:" not in text, name
        assert "https:" not in text, name
        # A URL that starts with // keeps the scheme of the page and
        # names another host.
        assert not re.search(r"""["'(=]\s*//""", text), name
    css = source("app.css")
    assert "@import" not in css
    assert "url(" not in css


# What parses a string as markup or as code.
SINKS = ("innerHTML", "outerHTML", "insertAdjacentHTML", "document.write",
         "createContextualFragment", "srcdoc", "eval(", "new Function",
         "setTimeout(\"", "setTimeout('", "setInterval(\"",
         "setInterval('")


def test_no_html_parsing_sinks_in_js():
    js = source("app.js")
    for sink in SINKS:
        assert sink not in js, sink


def test_no_inline_code_in_html():
    for tag, attrs in parse():
        assert tag != "style", "style element"
        if tag == "script":
            assert "src" in attrs, "script element without src"
        assert "style" not in attrs, (tag, attrs)
        for name in attrs:
            assert not name.startswith("on"), (tag, name)


def test_ids_used_by_js_exist_in_html():
    js = source("app.js")
    used = re.findall(r"""getElementById\(\s*"([^"]+)"\s*\)""", js)
    assert used
    # Every call names its id as a literal, except the lookups of the
    # error element of a field, which go through FIELD_ERRORS.
    through_table = js.count("getElementById(FIELD_ERRORS[")
    assert len(used) + through_table == js.count("getElementById(")
    table = re.search(r"FIELD_ERRORS = \{(.*?)\};", js, re.S).group(1)
    used += re.findall(r'\w+:\s*"([^"]+)"', table)
    ids = {a["id"] for _, a in parse() if "id" in a}
    assert set(used) <= ids


def test_one_request_helper():
    js = source("app.js")
    assert js.count("fetch(") == 1
    assert "application/json" in js


def test_switch_feedback_texts():
    js = source("app.js")
    for text in ("rsswitchd unreachable", "rsswitchd timed out",
                 "rsswitchd: ", "web app unreachable", "Never sent",
                 "Last sent: "):
        assert text in js, text


def test_page_says_last_sent_is_not_real_state():
    hint = [a for _, a in parse() if a.get("id") == "hint"]
    assert len(hint) == 1
    assert "not the real state" in source("index.html")


def test_css_has_dark_scheme_and_large_targets():
    css = source("app.css")
    assert "prefers-color-scheme: dark" in css
    assert "min-height: 48px" in css


def test_page_and_assets_need_login_when_a_password_is_set(make_app):
    client = make_app(sample_doc(), password_hash=HASH).test_client()
    for path in ("/", "/static/app.js", "/static/app.css"):
        assert client.get(path).status_code == 401, path
        resp = client.get(path, headers=basic("admin", "secret"))
        assert resp.status_code == 200, path


def test_missing_asset_is_json_404(client):
    resp = client.get("/static/missing.js")
    assert resp.status_code == 404
    assert resp.get_json()["error"]["code"] == "not_found"


def has_security_headers(resp):
    for name, value in SECURITY_HEADERS.items():
        assert resp.headers.get(name) == value, name


def test_security_headers_values():
    assert SECURITY_HEADERS == {
        "Content-Security-Policy": (
            "default-src 'self'; base-uri 'none'; form-action 'self'; "
            "frame-ancestors 'none'; object-src 'none'"),
        "X-Content-Type-Options": "nosniff",
        "Referrer-Policy": "no-referrer",
        "X-Frame-Options": "DENY",
    }


@pytest.mark.parametrize("path", ["/", "/static/app.js", "/static/app.css",
                                  "/api/rooms", "/api/nothing-here",
                                  "/static/missing.js"])
def test_every_response_has_the_security_headers(client, path):
    has_security_headers(client.get(path))


def test_errors_have_the_security_headers(make_app):
    locked = make_app(sample_doc(), password_hash=HASH).test_client()
    resp = locked.get("/")
    assert resp.status_code == 401
    has_security_headers(resp)
    resp = make_app(sample_doc()).test_client().post(
        "/api/rooms", data="{}", content_type="text/plain")
    assert resp.status_code == 415
    has_security_headers(resp)


def test_api_answers_are_never_cached(client):
    ok = client.get("/api/rooms")
    assert ok.status_code == 200
    assert ok.headers["Cache-Control"] == "no-store"
    missing = client.get("/api/nothing-here")
    assert missing.status_code == 404
    assert missing.headers["Cache-Control"] == "no-store"
    bad = client.post("/api/rooms", data="{}", content_type="text/plain")
    assert bad.headers["Cache-Control"] == "no-store"


def test_assets_are_revalidated_and_answer_304(client):
    first = client.get("/static/app.js")
    assert first.headers["Cache-Control"] == "no-cache"
    etag = first.headers["ETag"]
    again = client.get("/static/app.js", headers={"If-None-Match": etag})
    assert again.status_code == 304
    has_security_headers(again)


def test_page_reloads_when_shown_again_and_does_not_poll():
    js = source("app.js")
    assert "visibilitychange" in js
    assert "setInterval" not in js
    # No timer starts a reload. The one timer of the page only ends the
    # flash of a row.
    for timer in re.findall(r"setTimeout\((.*)", js):
        assert "loadTree" not in timer


def test_page_has_all_off_buttons():
    ids = [a["id"] for _, a in parse() if "id" in a]
    assert "all-off" in ids
    js = source("app.js")
    for text in ("/all-off", "Switch off all ", "All off: ", "skipped: "):
        assert text in js, text
    # The household asks once, with the number of sockets.
    assert js.count("Switch off all ") == 1


SOCKET_DIALOG_IDS = (
    "edit-toggle", "socket-dialog", "socket-form", "socket-dialog-title",
    "socket-name", "socket-type", "socket-group", "socket-number",
    "socket-room", "socket-name-error", "socket-type-error",
    "socket-group-error", "socket-number-error", "socket-room-error",
    "socket-dialog-error", "socket-cancel", "socket-save",
    "socket-address-note")


def test_socket_dialog_has_its_elements():
    by_id = {a["id"]: (t, a) for t, a in parse() if "id" in a}
    for name in SOCKET_DIALOG_IDS:
        assert name in by_id, name
    assert by_id["socket-dialog"][0] == "dialog"
    assert by_id["socket-form"][1]["method"] == "dialog"
    assert by_id["edit-toggle"][1]["aria-pressed"] == "false"
    assert "disabled" in by_id["edit-toggle"][1]
    for name in ("group", "number"):
        attrs = by_id["socket-" + name][1]
        assert attrs["type"] == "number"
        assert attrs["min"] == "0"
        assert attrs["step"] == "1"
        assert attrs["inputmode"] == "numeric"
        assert "required" in attrs


def test_socket_limits_come_from_the_server_not_the_html():
    # The max of group and number is set by the script, from the limits
    # that GET /api/meta serves, so the page has no copy of them.
    for tag, attrs in parse():
        if attrs.get("id") in ("socket-group", "socket-number"):
            assert "max" not in attrs, attrs
    js = source("app.js")
    for text in ("/api/meta", "groups - 1", "sockets - 1",
                 "reportValidity", "valueAsNumber", "Same address as: "):
        assert text in js, text


def test_inputs_do_not_make_a_phone_zoom():
    css = source("app.css")
    assert "font-size: 16px" in css
    assert "min-height: 44px" in css


ROOM_DIALOG_IDS = (
    "add-room", "room-dialog", "room-form", "room-dialog-title",
    "room-name", "room-name-error", "room-dialog-error", "room-cancel",
    "room-save")


def test_room_dialog_has_its_elements():
    by_id = {a["id"]: (t, a) for t, a in parse() if "id" in a}
    for name in ROOM_DIALOG_IDS:
        assert name in by_id, name
    assert by_id["room-dialog"][0] == "dialog"
    assert by_id["room-form"][1]["method"] == "dialog"
    name = by_id["room-name"][1]
    assert name["maxlength"] == "64"
    assert "required" in name
    assert "hidden" in by_id["add-room"][1]


def test_room_delete_asks_first_and_explains_a_refusal():
    js = source("app.js")
    assert 'Delete room "' in js
    assert "confirm(" in js
    assert "roomNotes" in js
    # The question comes before the request.
    body = js[js.index("async function deleteRoom"):]
    assert body.index("confirm(") < body.index('"DELETE"')
