"""HTTP face of the backend: the API, the HTTP ingest adapter and the dashboard files."""

import ipaddress
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, Request
from fastapi.concurrency import run_in_threadpool
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

from .all_clear_api import add_all_clear_routes
from .backend import REJECTED
from .health_api import health_router
from .officer_api import add_officer_routes

STATIC_DIR = Path(__file__).resolve().parent / "static"


HTTP_INGEST_SETTING = "WADIWATCH_HTTP_INGEST"


class HttpIngestRefused(ValueError):
    """The HTTP ingest endpoint was asked for where it must not be served."""


def http_ingest_setting(value, host):
    """Whether the backend is to serve its HTTP ingest endpoint.

    The endpoint takes gateway messages from anyone who can reach it, and the
    all clear is judged on gateway messages. So it is off unless
    WADIWATCH_HTTP_INGEST is exactly "1" (`value`), and even then it is refused
    for a backend that listens anywhere but on the local machine (`host`).
    """
    if value != "1":
        return False
    if not _is_local(host):
        raise HttpIngestRefused(
            f"{HTTP_INGEST_SETTING}=1 is for a local replay only: the HTTP ingest endpoint is "
            f"not authenticated, and this backend is set to listen on {host}. Leave "
            "WADIWATCH_HOST at 127.0.0.1, or feed the backend over MQTT."
        )
    return True


def _is_local(host):
    if host == "localhost":
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def create_app(backend, uplink=None, secure_cookies=False, http_ingest=False):
    """Build the web application over a Backend.

    `uplink`, if given, is started and stopped with the application (the MQTT
    subscriber in production; nothing in tests). `secure_cookies` marks the
    officers' session cookie for HTTPS only. `http_ingest` adds the HTTP ingest
    endpoint, which is not authenticated: for a local replay only.
    """

    @asynccontextmanager
    async def lifespan(app):
        if uplink is not None:
            uplink.start()
        try:
            yield
        finally:
            if uplink is not None:
                uplink.stop()

    app = FastAPI(title="WadiWatch backend", lifespan=lifespan)
    # Alert log, sign-in and acknowledgement (ticket #13) are in officer_api.py.
    add_officer_routes(app, backend, secure_cookies=secure_cookies)
    # The all clear (ticket #14) is in all_clear_api.py.
    add_all_clear_routes(app, backend)

    @app.get("/api/nodes")
    def nodes():
        return backend.nodes()

    # Curves, node health and staleness.
    app.include_router(health_router(backend))

    if http_ingest:

        @app.post("/api/ingest")
        async def ingest(request: Request):
            """Same ingest as the MQTT uplink, for the replay tool.

            The body is one gateway message. This endpoint is not authenticated,
            so it exists only in a backend started for a local replay.
            """
            payload = await request.body()
            result = await run_in_threadpool(backend.ingest, payload)
            if result.status == REJECTED:
                return JSONResponse(
                    {"status": result.status, "error": result.error}, status_code=400
                )
            return {"status": result.status}

    @app.get("/", include_in_schema=False)
    def dashboard():
        return FileResponse(STATIC_DIR / "index.html")

    app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")
    return app
