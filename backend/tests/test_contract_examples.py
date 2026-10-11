"""The examples published with the contract document are what the backend accepts.

docs/gateway-contract/gateway-output.jsonl is the shared contract check. The
firmware tests fail unless it is, byte for byte, what the gateway core hands to
its uplink (firmware/test/test_gateway_contract); the tests here fail unless
the backend ingests every line of it. valid.jsonl holds further examples,
written by hand.
"""

import json

import pytest

from conftest import backend_commands, gateway_output, valid_examples
from wadiwatch_backend.contract import KINDS
from wadiwatch_backend.downlink import TEST_COMMAND_KEY, all_clear_command, parse_command


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
    # The gateway relayed an all clear for every wadi node, each naming the command it
    # came from, so no alert is left in force: not at a wadi node, and not at the siren
    # node, whose acknowledgement of the last alert refresh arrives behind the all clear.
    all_clears = [m for m in messages if m["kind"] == "all_clear"]
    assert {m["wadi_node"] for m in all_clears} == wadi_node_ids
    assert all(m["node"] == site.gateway.id and m["command"] >= 1 for m in all_clears)
    assert {node["alert_level"] for node in nodes.values()} == {"none"}


# What the backend sends down: docs/gateway-contract/backend-commands.jsonl, signed with
# the published test command key. The firmware tests fail unless the gateway core,
# holding that key, reads every line of it, and take the command of their contract
# scenario from it. So the two sides also agree on the test key and on how a command's
# authentication code is computed.


@pytest.mark.parametrize("raw", backend_commands())
def test_command_of_the_shared_examples_is_what_the_backend_sends_byte_for_byte(raw):
    command = parse_command(raw, TEST_COMMAND_KEY)

    assert (
        all_clear_command(command["command"], command["ts"], command["wadi"], TEST_COMMAND_KEY)
        == raw
    )


def test_the_shared_commands_are_the_ones_the_gateway_output_answers(site):
    commands = [json.loads(raw) for raw in backend_commands()]
    relayed = {m["command"] for m in map(json.loads, gateway_output()) if m["kind"] == "all_clear"}

    assert commands
    assert all(site.wadi(command["wadi"]) is not None for command in commands)
    assert relayed == {command["command"] for command in commands}


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
