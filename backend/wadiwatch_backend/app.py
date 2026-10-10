"""HTTP face of the backend: the API, the HTTP ingest adapter and the dashboard files."""

from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, Request
from fastapi.concurrency import run_in_threadpool
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

from .backend import REJECTED
from .officer_api import add_officer_routes

STATIC_DIR = Path(__file__).resolve().parent / "static"


def create_app(backend, uplink=None, secure_cookies=False):
    """Build the web application over a Backend.

    `uplink`, if given, is started and stopped with the application (the MQTT
    subscriber in production; nothing in tests). `secure_cookies` marks the
    officers' session cookie for HTTPS only.
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

    @app.get("/api/nodes")
    def nodes():
        return backend.nodes()

    @app.post("/api/ingest")
    async def ingest(request: Request):
        """Same ingest as the MQTT uplink, for the replay tool and for tests.

        The body is one gateway message. This endpoint is not authenticated:
        keep the backend bound to the local machine.
        """
        payload = await request.body()
        result = await run_in_threadpool(backend.ingest, payload)
        if result.status == REJECTED:
            return JSONResponse({"status": result.status, "error": result.error}, status_code=400)
        return {"status": result.status}

    @app.get("/", include_in_schema=False)
    def dashboard():
        return FileResponse(STATIC_DIR / "index.html")

    app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")
    return app
