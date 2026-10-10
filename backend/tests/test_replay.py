"""The replay tool feeds the recorded flood scenario through the HTTP ingest endpoint."""

import json

import pytest

from wadiwatch_backend.replay import DEFAULT_SCENARIO, load_scenario, replay


@pytest.fixture
def scenario():
    return load_scenario(DEFAULT_SCENARIO)


class Recorder:
    """Sends through the HTTP endpoint and notes what the map shows after each message."""

    def __init__(self, harness):
        self.harness = harness
        self.sent = []
        self.levels_shown = {node_id: ["none"] for node_id in harness.nodes()}

    def __call__(self, payload):
        self.sent.append(json.loads(payload))
        response = self.harness.client.post("/api/ingest", content=payload)
        for node_id, node in self.harness.nodes().items():
            if self.levels_shown[node_id][-1] != node["alert_level"]:
                self.levels_shown[node_id].append(node["alert_level"])
        return response.json()["status"]


def test_flood_scenario_takes_the_map_through_a_whole_event(
    harness, scenario, clock, upstream, downstream, siren
):
    send = Recorder(harness)

    summary = replay(scenario, send, instant=True, clock=clock)

    assert summary.stored == len(scenario)
    assert summary.rejected == 0
    assert summary.duplicate == 0
    assert send.levels_shown[upstream.id] == ["none", "watch", "warning", "none"]
    # Warning here, confirmed by Warning upstream, becomes Evacuate.
    assert send.levels_shown[downstream.id] == ["none", "watch", "warning", "evacuate", "none"]
    assert send.levels_shown[siren.id] == ["none", "warning", "evacuate", "none"]


def test_flood_scenario_uses_every_message_kind_and_only_configured_nodes(scenario, site):
    assert {message["kind"] for message in scenario} == {
        "reading",
        "alert",
        "acknowledgement",
        "heartbeat",
        "all_clear",
        "test",
    }
    configured = {node.id for node in site.nodes} | {site.gateway.id}
    assert {message["node"] for message in scenario} <= configured


def test_instant_replay_ends_now_and_keeps_the_recorded_spacing(harness, scenario, clock):
    send = Recorder(harness)

    replay(scenario, send, instant=True, clock=clock)

    recorded = [message["ts"] for message in scenario]
    replayed = [message["ts"] for message in send.sent]
    assert replayed[-1] == int(clock())
    assert [ts - replayed[0] for ts in replayed] == [ts - recorded[0] for ts in recorded]


def test_timed_replay_is_compressed_and_stamps_each_message_when_it_is_sent(
    harness, scenario, clock
):
    send = Recorder(harness)
    started = clock()
    speed = 120.0
    stamped_at = []

    def send_and_note_time(payload):
        stamped_at.append(int(clock()))
        return send(payload)

    replay(scenario, send_and_note_time, speed=speed, clock=clock, sleep=clock.advance)

    duration = scenario[-1]["ts"] - scenario[0]["ts"]
    assert clock() - started == pytest.approx(duration / speed)
    assert [message["ts"] for message in send.sent] == stamped_at


def test_replaying_twice_adds_a_second_event_instead_of_duplicates(harness, scenario, clock):
    send = Recorder(harness)
    replay(scenario, send, instant=True, clock=clock)
    clock.advance(86400)

    summary = replay(scenario, send, instant=True, clock=clock)

    assert summary.stored == len(scenario)
    assert summary.duplicate == 0
