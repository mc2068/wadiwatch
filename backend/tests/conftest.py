"""Test harness for the backend-ingest seam.

Tests feed gateway messages into the one ingest function and read the result
back through the HTTP API, as the dashboard does. Time comes from a fake clock.
Node identifiers are read from the site file, never written in a test.
"""

import json
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from wadiwatch_backend.app import create_app
from wadiwatch_backend.backend import Backend
from wadiwatch_backend.contract import CONTRACT_VERSION
from wadiwatch_backend.site import DEFAULT_SITE_FILE, load_site

# Fixtures for sign-in and the alert log (visitor, officer, signed_in, ...) are in officer_support.py.
pytest_plugins = ["officer_support"]

REPO_ROOT = Path(__file__).resolve().parents[2]
CONTRACT_EXAMPLES = REPO_ROOT / "docs" / "gateway-contract"

# 2026-10-10 00:00:00 UTC. Any fixed instant would do.
TEST_EPOCH = 1_791_590_400


class FakeClock:
    def __init__(self, now):
        self.now = now

    def __call__(self):
        return self.now

    def advance(self, seconds):
        self.now += seconds


class Harness:
    """One backend over one database file, restartable like the real process."""

    def __init__(self, db_path, site, clock):
        self.db_path = db_path
        self.site = site
        self.clock = clock
        self._counters = {}
        self.backend = None
        self.restart()

    def restart(self):
        """Stop the running backend and start a new one on the same file."""
        self.stop()
        self.backend = Backend(self.db_path, self.site, clock=self.clock)
        self.client = TestClient(create_app(self.backend))

    def stop(self):
        if self.backend is not None:
            self.backend.close()
            self.backend = None

    def message(self, kind, node, **fields):
        """A well-formed gateway message, stamped with the fake clock."""
        counter = self._counters.get(node, 0) + 1
        self._counters[node] = counter
        message = {
            "v": CONTRACT_VERSION,
            "kind": kind,
            "node": node,
            "counter": counter,
            "ts": int(self.clock()),
        }
        message.update(fields)
        return message

    def ingest(self, kind, node, **fields):
        return self.ingest_raw(json.dumps(self.message(kind, node, **fields)))

    def ingest_raw(self, payload):
        return self.backend.ingest(payload)

    def api(self, path):
        response = self.client.get(path)
        assert response.status_code == 200, response.text
        return response.json()

    def nodes(self):
        """The nodes API, keyed by node identifier."""
        return {node["id"]: node for node in self.api("/api/nodes")["nodes"]}


@pytest.fixture
def site():
    return load_site(DEFAULT_SITE_FILE)


@pytest.fixture
def clock():
    return FakeClock(TEST_EPOCH)


@pytest.fixture
def harness(tmp_path, site, clock):
    harness = Harness(tmp_path / "wadiwatch.db", site, clock)
    yield harness
    harness.stop()


@pytest.fixture
def upstream(site):
    """The wadi node with nothing upstream of it."""
    return next(node for node in site.wadi_nodes if node.upstream is None)


@pytest.fixture
def downstream(site, upstream):
    """The wadi node directly downstream of `upstream`."""
    return next(node for node in site.wadi_nodes if node.upstream == upstream.id)


@pytest.fixture
def siren(site):
    return site.siren_nodes[0]


def read_jsonl(path):
    with open(path, encoding="utf-8") as lines:
        return [line.rstrip("\n") for line in lines if line.strip()]


def valid_examples():
    return read_jsonl(CONTRACT_EXAMPLES / "valid.jsonl")


def invalid_examples():
    """(why, raw payload) for every documented malformed message."""
    cases = [json.loads(line) for line in read_jsonl(CONTRACT_EXAMPLES / "invalid.jsonl")]
    return [(case["why"], case["raw"]) for case in cases]
