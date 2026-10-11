"""HTTP routes for the alert log, officer sign-in and acknowledgement.

    GET    /api/alert-log                        public; officers' names only for a signed-in officer
    GET    /api/session                          who is signed in in this browser, and the CSRF token
    POST   /api/session                          sign in: {"name": ..., "password": ...}
    DELETE /api/session                          sign out
    POST   /api/alerts/{id}/acknowledgement      acknowledge an alert of the alert log

How a request that changes something is protected:

- It needs the session cookie, which is HttpOnly (scripts cannot read it) and
  SameSite=Strict (browsers do not send it with requests made by other sites).
- It needs the session's CSRF token in the X-CSRF-Token header. Only a script
  on the dashboard's own origin can read the token (from /api/session) and
  set that header.
- If the browser says where the request comes from (the Origin header), that
  must be the dashboard's own address.

Sign-in has no session yet, so instead of the token it must be sent as JSON,
which a form on another site cannot do.

Every officer-only action is guarded with `officer_session`, as the
acknowledgement route here and the all clear route (all_clear_api.py) are.
"""

import hmac
import json
from urllib.parse import urlsplit

from fastapi import FastAPI, Request
from fastapi.concurrency import run_in_threadpool
from fastapi.responses import JSONResponse

from .backend import UnknownAlert
from .officers import SESSION_SECONDS

SESSION_COOKIE = "wadiwatch_session"
CSRF_HEADER = "X-CSRF-Token"
MAX_SIGN_IN_BYTES = 4096

# Answers here depend on who asks: nothing in between may keep a copy.
NO_STORE = {"Cache-Control": "no-store"}


class Refused(Exception):
    def __init__(self, status_code, error):
        super().__init__(error)
        self.status_code = status_code
        self.error = error


def answer(content, status_code=200):
    """A JSON answer that nothing in between may keep a copy of."""
    return JSONResponse(content, status_code=status_code, headers=NO_STORE)


def _refuse_other_sites(request):
    origin = request.headers.get("origin")
    if origin is not None and urlsplit(origin).netloc != request.headers.get("host"):
        raise Refused(403, "This request did not come from the dashboard")


def officer_session(request, officers, action):
    """The session of the officer making a state-changing request, or raise Refused.

    `action` completes the sentence "Sign in to ...".
    """
    _refuse_other_sites(request)
    session = officers.session(request.cookies.get(SESSION_COOKIE))
    if session is None:
        raise Refused(401, f"Sign in to {action}")
    sent = request.headers.get(CSRF_HEADER, "")
    # Compared as bytes: compare_digest refuses text with characters outside ASCII.
    if not hmac.compare_digest(sent.encode("utf-8"), session.csrf_token.encode("utf-8")):
        raise Refused(403, "The CSRF token is missing or wrong; reload the dashboard")
    return session


def add_officer_routes(app: FastAPI, backend, secure_cookies=False):
    officers = backend.officers

    @app.exception_handler(Refused)
    async def refused(request, refusal):
        return answer({"error": refusal.error}, refusal.status_code)

    def signed_in(session):
        return {"signed_in": True, "officer": session.officer, "csrf_token": session.csrf_token}

    @app.get("/api/alert-log")
    def alert_log(request: Request):
        session = officers.session(request.cookies.get(SESSION_COOKIE))
        return answer(
            {
                "signed_in_as": session.officer if session else None,
                "entries": backend.alert_log(show_officers=session is not None),
            }
        )

    @app.get("/api/session")
    def session_state(request: Request):
        session = officers.session(request.cookies.get(SESSION_COOKIE))
        return answer(signed_in(session) if session else {"signed_in": False})

    @app.post("/api/session")
    async def sign_in(request: Request):
        _refuse_other_sites(request)
        content_type = request.headers.get("content-type", "").split(";")[0].strip().lower()
        if content_type != "application/json":
            raise Refused(415, "Send the name and password as JSON")
        body = await request.body()
        credentials = None
        if len(body) <= MAX_SIGN_IN_BYTES:
            try:
                credentials = json.loads(body)
            except (ValueError, RecursionError):
                pass
        if not isinstance(credentials, dict):
            credentials = {}
        result = await run_in_threadpool(
            officers.sign_in, credentials.get("name"), credentials.get("password")
        )
        if result is None:
            raise Refused(401, "Wrong name or password")
        token, session = result
        # A browser that signs in again does not leave its old session behind.
        officers.sign_out(request.cookies.get(SESSION_COOKIE))
        response = answer(signed_in(session))
        response.set_cookie(
            SESSION_COOKIE,
            token,
            max_age=SESSION_SECONDS,
            path="/",
            httponly=True,
            samesite="strict",
            secure=secure_cookies,
        )
        return response

    @app.delete("/api/session")
    def sign_out(request: Request):
        # Checked even with no session: another site must not be able to clear the cookie.
        _refuse_other_sites(request)
        token = request.cookies.get(SESSION_COOKIE)
        if officers.session(token) is not None:
            officer_session(request, officers, "sign out")
            officers.sign_out(token)
        response = answer({"signed_in": False})
        response.delete_cookie(
            SESSION_COOKIE, path="/", httponly=True, samesite="strict", secure=secure_cookies
        )
        return response

    @app.post("/api/alerts/{alert_id}/acknowledgement")
    def acknowledge(alert_id: int, request: Request):
        session = officer_session(request, officers, "acknowledge an alert")
        try:
            acknowledgement = backend.acknowledge_alert(alert_id, session.officer)
        except UnknownAlert:
            raise Refused(404, "There is no such alert in the alert log") from None
        return answer(
            {
                "alert": alert_id,
                "acknowledgement": {"ts": acknowledgement.ts, "officer": acknowledgement.officer},
            }
        )
