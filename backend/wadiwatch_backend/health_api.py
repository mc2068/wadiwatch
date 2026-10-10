"""API routes for the curves, node health and staleness."""

from fastapi import APIRouter, HTTPException, Query

from .backend import EmptyPeriod, NotAWadiNode
from .contract import MAX_UINT32


def health_router(backend):
    router = APIRouter()

    @router.get("/api/health")
    def health():
        return backend.health()

    @router.get("/api/nodes/{node_id}/readings")
    def readings(
        node_id: int,
        start: int | None = Query(
            None, alias="from", ge=0, le=MAX_UINT32, description="Unix seconds; a day before `to` if left out"
        ),
        end: int | None = Query(
            None, alias="to", ge=0, le=MAX_UINT32, description="Unix seconds; now if left out"
        ),
    ):
        try:
            return backend.readings(node_id, start, end)
        except NotAWadiNode:
            raise HTTPException(404, f"node {node_id} is not a wadi node of the site") from None
        except EmptyPeriod as error:
            raise HTTPException(400, str(error)) from None

    return router
