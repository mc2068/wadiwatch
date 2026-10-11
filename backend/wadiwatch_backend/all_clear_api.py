"""HTTP routes for the all clear.

    GET  /api/wadis                    public: for every wadi, whether an all clear would be
                                       accepted now and, if not, why
    POST /api/wadis/{id}/all-clear     a signed-in officer issues the all clear for a wadi

The POST is an officer-only action and is guarded exactly as the
acknowledgement is (officer_api.officer_session): session cookie, CSRF token,
own origin. What the backend then checks is in all_clear.py; what it sends the
gateway is in downlink.py.

    200  {"wadi", "command", "ts", "officer"}  the command is on its way to the gateway
    401  not signed in
    403  CSRF token missing or wrong, or the request came from another site
    404  the site has no such wadi
    409  refused: the level has not been below the Watch threshold for long
         enough, or the data is missing or stale. The answer carries the
         verdict of GET /api/wadis for that wadi beside the error.
    503  the command could not be sent towards the gateway

A 200 says the command was sent, not that the sirens have stopped: the all
clear shows in the alert log when the gateway reports having transmitted it.
"""

from fastapi import FastAPI, Request

from .backend import AllClearRefused, GatewayUnreachable, UnknownWadi
from .officer_api import Refused, answer, officer_session


def add_all_clear_routes(app: FastAPI, backend):
    @app.get("/api/wadis")
    def wadis():
        return answer(backend.all_clear_verdicts())

    @app.post("/api/wadis/{wadi_id}/all-clear")
    def issue_all_clear(wadi_id: int, request: Request):
        session = officer_session(request, backend.officers, "issue an all clear")
        try:
            command = backend.issue_all_clear(wadi_id, session.officer)
        except UnknownWadi:
            raise Refused(404, "The site has no such wadi") from None
        except AllClearRefused as refused:
            return answer(
                {"error": f"All clear refused. {refused.verdict['refusal']}", **refused.verdict},
                status_code=409,
            )
        except GatewayUnreachable:
            raise Refused(
                503, "The all clear was not sent: the backend has no connection to the gateway"
            ) from None
        return answer(command)
