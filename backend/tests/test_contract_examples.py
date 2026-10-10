"""The examples published with the contract document are what the backend accepts.

docs/gateway-contract/gateway-output.jsonl is the shared contract check. The
firmware tests fail unless it is, byte for byte, what the gateway core hands to
its uplink (firmware/test/test_gateway_contract); the tests here fail unless
the backend ingests every line of it. valid.jsonl holds further examples,
written by hand.
"""

import json

import pytest

from conftest import gateway_output, valid_examples
from wadiwatch_backend.contract import KINDS


@pytest.mark.parametrize("raw", gateway_output())
def test_message_produced_by_the_gateway_core_is_accepted(harness, raw):
    result = harness.ingest_raw(raw)

    assert result.status == "stored", result.error


def test_gateway_output_covers_every_message_kind():
    produced = {json.loads(raw)["kind"] for raw in gateway_output()}

    assert produced == set(KINDS)


def test_gateway_output_ingested_in_order_shows_through_the_api(harness, site):
    messages = [json.loads(raw) for raw in gateway_output()]
    for raw in gateway_output():
        harness.ingest_raw(raw)

    nodes = harness.nodes()

    wadi_node_ids = {node.id for node in site.wadi_nodes}
    last_reading = {m["node"]: m for m in messages if m["kind"] == "reading"}
    assert last_reading
    for node_id, reading in last_reading.items():
        assert nodes[node_id]["level_cm"] == reading["level_cm"]
        assert nodes[node_id]["rise_cm_min"] == reading["rise_cm_min"]
        assert nodes[node_id]["level_ts"] == reading["ts"]
    # The file ends with the all clear, so no alert is left in force.
    assert messages[-1]["kind"] == "all_clear"
    assert messages[-1]["wadi_node"] in wadi_node_ids
    assert {node["alert_level"] for node in nodes.values()} == {"none"}


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
