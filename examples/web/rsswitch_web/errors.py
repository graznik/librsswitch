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

"""Every error response has the body {"error": {"code", "message"}}.

The code is a fixed word that clients may branch on. The message is
free text and may change, like the text of an rsswitchd ERR reply.
"""

from __future__ import annotations

import json

from flask import jsonify, request
from werkzeug.exceptions import HTTPException, MethodNotAllowed

from .store import StoreError

# Request bodies are small JSON objects. Werkzeug answers 413 for more.
MAX_BODY_BYTES = 16 * 1024

# The code of every HTTP status the API knows about. Any other status
# keeps its number and gets the code http_error.
HTTP_CODES = {
    400: "bad_json",
    401: "unauthorized",
    404: "not_found",
    405: "method_not_allowed",
    413: "payload_too_large",
    415: "unsupported_media_type",
}

_STORE_ERROR_MESSAGE = "the data store is unavailable"
_INTERNAL_ERROR_MESSAGE = "internal server error"


class ApiError(Exception):
    """Abort a request with an error response.

    field names the request field a validation error is about. Any
    further keyword arguments become more keys of the error object.
    """

    def __init__(self, status, code, message, *, field=None, headers=None,
                 **extra):
        super().__init__(message)
        self.status = status
        self.code = code
        self.message = message
        self.headers = headers
        self.extra = {}
        if field is not None:
            self.extra["field"] = field
        self.extra.update(extra)


def error_response(status, code, message, headers=None, **extra):
    """Build the JSON error response."""
    error = {"code": code, "message": message}
    error.update(extra)
    resp = jsonify({"error": error})
    resp.status_code = status
    for name, value in (headers or {}).items():
        resp.headers[name] = value
    return resp


def _handle_api_error(e):
    return error_response(e.status, e.code, e.message, e.headers,
                          **e.extra)


def _handle_http_exception(e):
    status = e.code or 500
    if status == 500:
        return error_response(500, "internal_error",
                              _INTERNAL_ERROR_MESSAGE)
    headers = {name: value for name, value in e.get_headers()
               if name.lower() not in ("content-type", "content-length")}
    if isinstance(e, MethodNotAllowed) and e.valid_methods:
        headers["Allow"] = ", ".join(sorted(e.valid_methods))
    code = HTTP_CODES.get(status, "http_error")
    message = e.description or e.name
    return error_response(status, code, message, headers)


def register_error_handlers(app):
    """Answer every error of app with the JSON error contract."""

    @app.errorhandler(ApiError)
    def api_error(e):
        return _handle_api_error(e)

    @app.errorhandler(StoreError)
    def store_error(e):
        # The text names the data file. It belongs in the log only.
        app.logger.error("%s", e)
        return error_response(500, "store_error", _STORE_ERROR_MESSAGE)

    @app.errorhandler(HTTPException)
    def http_exception(e):
        return _handle_http_exception(e)

    @app.errorhandler(Exception)
    def unexpected(e):
        app.logger.exception("unhandled exception")
        return error_response(500, "internal_error",
                              _INTERNAL_ERROR_MESSAGE)


def _reject_constant(name):
    raise ValueError("invalid JSON constant %s" % name)


def json_body(required=True) -> dict:
    """Return the request body as a dict.

    A blank body is an error when required and {} otherwise. Anything
    else must be one JSON object. The get_json helper of the request is
    not used: it accepts lists and NaN, and has its own idea of which
    status to answer.
    """
    raw = request.get_data()
    if not raw.strip():
        if required:
            raise ApiError(400, "bad_json", "request body is empty")
        return {}
    try:
        body = json.loads(raw.decode("utf-8"),
                          parse_constant=_reject_constant)
    except (ValueError, RecursionError):
        raise ApiError(400, "bad_json", "request body is not valid JSON")
    if not isinstance(body, dict):
        raise ApiError(400, "bad_json", "request body must be a JSON object")
    return body


def reject_unknown_fields(body, allowed):
    """Raise a 422 naming the first field of body that is not allowed."""
    unknown = sorted(k for k in body if k not in allowed)
    if unknown:
        raise ApiError(422, "validation", "unknown field", field=unknown[0])
