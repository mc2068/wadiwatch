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
from wadiwatch_backend.downlink import TEST_COMMAND_KEY, parse_command
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


class GatewayLink:
    """The way down to the gateway, as the backend sees it.

    It records the commands handed over and does nothing with them: a test
    says what the gateway does next (`Harness.gateway_relays`). A command that
    is not worded as the contract words it, or does not carry the
    authentication code of the gateway's command key, fails the test that
    sent it.
    """

    def __init__(self, command_key=TEST_COMMAND_KEY):
        # The key this gateway shares with the backend.
        self.command_key = command_key
        # False: the broker cannot be reached, nothing is handed over.
        self.reachable = True
        # The commands handed over, as sent and as read back.
        self.raw = []
        self.commands = []

    def __call__(self, payload):
        if not self.reachable:
            return False
        self.commands.append(parse_command(payload, self.command_key))
        self.raw.append(payload)
        return True


class Harness:
    """One backend over one database file, restartable like the real process."""

    def __init__(self, db_path, site, clock, command_key=TEST_COMMAND_KEY):
        self.db_path = db_path
        self.site = site
        self.clock = clock
        # Whether the backend serves its HTTP ingest endpoint: off, as it starts
        # unless asked. The `ingest_over_http` fixture switches it on.
        self.http_ingest = False
        # The key the backend signs its commands with, and its gateway checks them against.
        self.command_key = command_key
        self._counters = {}
        self.backend = None
        # Outlives a restart of the backend, as the broker and the gateway do.
        self.gateway = GatewayLink(command_key)
        self.restart()

    def restart(self):
        """Stop the running backend and start a new one on the same file."""
        self.stop()
        self.backend = Backend(
            self.db_path,
            self.site,
            clock=self.clock,
            downlink=self.gateway,
            command_key=self.command_key,
        )
        self.client = TestClient(create_app(self.backend, http_ingest=self.http_ingest))

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

    def gateway_relays(self, command):
        """The gateway acts on an all clear command and reports it, now.

        `command` is one of `gateway.commands`. The gateway transmits an all
        clear packet for each wadi node of the wadi and sends one `all_clear`
        message for each, carrying the command's number.
        """
        for node in self.site.wadi_nodes_of(command["wadi"]):
            result = self.ingest(
                "all_clear", self.site.gateway.id, wadi_node=node.id, command=command["command"]
            )
            assert result.status == "stored", result.error

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
def ingest_over_http(harness):
    """The backend started with its HTTP ingest endpoint switched on, as for a local
    replay. Posts one payload to it and returns the response."""
    harness.http_ingest = True
    harness.restart()
    return lambda payload: harness.client.post("/api/ingest", content=payload)


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


@pytest.fixture
def wadi(site, upstream):
    """The wadi the site's wadi nodes are mounted over."""
    return site.wadi_of(upstream.id)


def read_jsonl(path):
    with open(path, encoding="utf-8") as lines:
        return [line.rstrip("\n") for line in lines if line.strip()]


def valid_examples():
    return read_jsonl(CONTRACT_EXAMPLES / "valid.jsonl")


def gateway_output():
    """The messages the gateway core produces, written by the firmware tests."""
    return read_jsonl(CONTRACT_EXAMPLES / "gateway-output.jsonl")


def firmware_site():
    """What the firmware site configuration says about the values the backend site
    file has a copy of, written by the firmware tests."""
    with open(CONTRACT_EXAMPLES / "firmware-site.json", encoding="utf-8") as site_file:
        return json.load(site_file)


def backend_commands():
    """The commands the backend sends down, which the firmware tests feed the gateway core."""
    return read_jsonl(CONTRACT_EXAMPLES / "backend-commands.jsonl")


def invalid_examples():
    """(why, raw payload) for every documented malformed message."""
    cases = [json.loads(line) for line in read_jsonl(CONTRACT_EXAMPLES / "invalid.jsonl")]
    return [(case["why"], case["raw"]) for case in cases]
