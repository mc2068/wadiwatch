"""Malformed gateway messages are rejected and leave what is stored untouched."""

import json

import pytest

from conftest import invalid_examples
from wadiwatch_backend.contract import MAX_MESSAGE_BYTES


@pytest.fixture
def event_in_progress(harness, upstream, downstream, siren):
    """Some stored state for a bad message to damage."""
    harness.ingest("reading", upstream.id, level_cm=118.0, rise_cm_min=4.5)
    harness.ingest("reading", downstream.id, level_cm=40.0, rise_cm_min=1.5)
    harness.ingest("alert", upstream.id, alert_level="warning")
    harness.ingest("acknowledgement", siren.id, wadi_node=upstream.id, alert_level="warning")
    return harness.api("/api/nodes")


def assert_rejected_without_trace(harness, before, payload):
    result = harness.ingest_raw(payload)

    assert result.status == "rejected"
    assert result.error
    assert harness.api("/api/nodes") == before
    harness.restart()
    assert harness.api("/api/nodes") == before


@pytest.mark.parametrize(
    "raw", [raw for _, raw in invalid_examples()], ids=[why for why, _ in invalid_examples()]
)
def test_documented_malformed_message_is_rejected(harness, event_in_progress, raw):
    assert_rejected_without_trace(harness, event_in_progress, raw)


def test_payload_that_is_not_utf8_is_rejected(harness, event_in_progress):
    assert_rejected_without_trace(harness, event_in_progress, b"\xff\xfe\x00{")


def test_oversized_payload_is_rejected(harness, event_in_progress, upstream):
    message = harness.message("reading", upstream.id, level_cm=1.0, rise_cm_min=0.0)
    padded = json.dumps(message) + " " * MAX_MESSAGE_BYTES

    assert_rejected_without_trace(harness, event_in_progress, padded)


def test_field_given_twice_is_rejected(harness, event_in_progress, upstream):
    message = json.dumps(harness.message("reading", upstream.id, level_cm=1.0, rise_cm_min=0.0))
    twice = message[:-1] + ',"level_cm":900.0}'

    assert_rejected_without_trace(harness, event_in_progress, twice)


def test_every_alert_level_other_than_the_four_names_is_rejected(
    harness, event_in_progress, upstream
):
    for bad_level in ("", "WARNING", "all_clear", "3", None, 2, True, ["warning"]):
        message = harness.message("alert", upstream.id, alert_level=bad_level)
        assert_rejected_without_trace(harness, event_in_progress, json.dumps(message))


def test_a_corrected_message_is_accepted_after_its_malformed_version(harness, upstream):
    good = harness.message("reading", upstream.id, level_cm=64.0, rise_cm_min=2.0)
    bad = dict(good, level_cm="64.0")

    assert harness.ingest_raw(json.dumps(bad)).status == "rejected"
    assert harness.ingest_raw(json.dumps(good)).status == "stored"
    assert harness.nodes()[upstream.id]["level_cm"] == 64.0


def test_rejection_says_what_is_wrong(harness, upstream):
    message = harness.message("reading", upstream.id, rise_cm_min=2.0)

    result = harness.ingest_raw(json.dumps(message))

    assert "level_cm" in result.error
