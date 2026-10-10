"""The examples published with the contract document are what the backend accepts.

docs/gateway-contract/valid.jsonl is the shared contract check: the gateway
firmware tests must produce messages of exactly this shape, and this test
proves the backend ingests them.
"""

import json

import pytest

from conftest import valid_examples
from wadiwatch_backend.contract import KINDS


@pytest.mark.parametrize("raw", valid_examples())
def test_documented_example_is_accepted(harness, raw):
    result = harness.ingest_raw(raw)

    assert result.status == "stored", result.error


def test_examples_cover_every_message_kind():
    documented = {json.loads(raw)["kind"] for raw in valid_examples()}

    assert documented == set(KINDS)


def test_examples_are_visible_through_the_api_once_ingested(harness):
    for raw in valid_examples():
        harness.ingest_raw(raw)
    examples = [json.loads(raw) for raw in valid_examples()]
    last_reading = {m["node"]: m for m in examples if m["kind"] == "reading"}

    nodes = harness.nodes()

    for node_id, reading in last_reading.items():
        assert nodes[node_id]["level_cm"] == reading["level_cm"]
        assert nodes[node_id]["rise_cm_min"] == reading["rise_cm_min"]
