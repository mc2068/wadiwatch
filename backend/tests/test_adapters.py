"""The HTTP endpoint and the MQTT subscriber are thin adapters over one ingest function."""

import json
from types import SimpleNamespace

from wadiwatch_backend.downlink import TEST_COMMAND_KEY, all_clear_command, command_auth_code
from wadiwatch_backend.mqtt import MqttUplink


def post(harness, payload):
    return harness.client.post("/api/ingest", content=payload)


def test_http_ingest_stores_a_message(harness, upstream):
    message = harness.message("reading", upstream.id, level_cm=73.5, rise_cm_min=2.5)

    response = post(harness, json.dumps(message))

    assert response.status_code == 200
    assert response.json() == {"status": "stored"}
    assert harness.nodes()[upstream.id]["level_cm"] == 73.5


def test_http_ingest_reports_a_duplicate_without_error(harness, upstream):
    payload = json.dumps(harness.message("alert", upstream.id, alert_level="watch"))
    post(harness, payload)

    response = post(harness, payload)

    assert response.status_code == 200
    assert response.json() == {"status": "duplicate"}


def test_http_ingest_refuses_a_malformed_message(harness, upstream):
    before = harness.nodes()

    response = post(harness, '{"kind": "reading"')

    assert response.status_code == 400
    assert response.json()["status"] == "rejected"
    assert response.json()["error"]
    assert harness.nodes() == before


def mqtt_message(payload):
    return SimpleNamespace(topic="wadiwatch/up", payload=payload)


def test_mqtt_message_is_ingested(harness, upstream):
    uplink = MqttUplink(harness.backend.ingest, host="broker.invalid")
    message = harness.message("alert", upstream.id, alert_level="evacuate")

    uplink.on_message(None, None, mqtt_message(json.dumps(message).encode()))

    assert harness.nodes()[upstream.id]["alert_level"] == "evacuate"


def test_malformed_mqtt_message_is_dropped_and_the_subscriber_keeps_going(harness, upstream):
    uplink = MqttUplink(harness.backend.ingest, host="broker.invalid")
    before = harness.nodes()

    uplink.on_message(None, None, mqtt_message(b"\x00\x01 not a message"))

    assert harness.nodes() == before
    good = harness.message("alert", upstream.id, alert_level="watch")
    uplink.on_message(None, None, mqtt_message(json.dumps(good).encode()))
    assert harness.nodes()[upstream.id]["alert_level"] == "watch"


# The way down: the backend's commands for the gateway


class FakeMqttClient:
    """As much of the MQTT client as sending a command touches."""

    def __init__(self, connected, rc=0):
        self.connected = connected
        self.rc = rc
        self.published = []

    def is_connected(self):
        return self.connected

    def publish(self, topic, payload, qos=0, retain=False):
        self.published.append({"topic": topic, "payload": payload, "qos": qos, "retain": retain})
        return SimpleNamespace(rc=self.rc)


def uplink_with(harness, client):
    uplink = MqttUplink(harness.backend.ingest, host="broker.invalid")
    uplink._client = client
    return uplink


def test_a_command_is_published_on_the_downlink_topic_at_least_once_and_not_retained(harness):
    client = FakeMqttClient(connected=True)
    command = all_clear_command(command=3, ts=int(harness.clock()), wadi=1, key=TEST_COMMAND_KEY)

    sent = uplink_with(harness, client).send_command(command)

    assert sent is True
    # Retained, the broker would hand the all clear to the gateway again at every reconnect.
    assert client.published == [
        {"topic": "wadiwatch/down", "payload": command, "qos": 1, "retain": False}
    ]


def test_a_command_is_not_handed_to_a_client_that_has_no_connection(harness):
    # The client library would keep it and send it whenever the broker came back.
    client = FakeMqttClient(connected=False)
    command = all_clear_command(command=3, ts=int(harness.clock()), wadi=1, key=TEST_COMMAND_KEY)

    assert uplink_with(harness, client).send_command(command) is False
    assert client.published == []
    # Nor before the adapter has been started at all.
    assert MqttUplink(harness.backend.ingest, host="broker.invalid").send_command(command) is False


def test_a_command_the_client_could_not_publish_is_reported_as_not_sent(harness):
    client = FakeMqttClient(connected=True, rc=4)
    command = all_clear_command(command=3, ts=int(harness.clock()), wadi=1, key=TEST_COMMAND_KEY)

    assert uplink_with(harness, client).send_command(command) is False


def test_the_adapter_sends_what_the_backend_issues(harness, signed_in, clock, site, wadi, upstream, downstream):
    client = FakeMqttClient(connected=True)
    harness.backend.downlink = uplink_with(harness, client).send_command
    for node in (upstream, downstream):
        harness.ingest("reading", node.id, level_cm=10.0, rise_cm_min=0.0)
    clock.advance(site.all_clear_quiet_s)
    for node in (upstream, downstream):
        harness.ingest("reading", node.id, level_cm=10.0, rise_cm_min=0.0)

    response = signed_in.issue_all_clear(wadi.id)

    assert response.status_code == 200
    (published,) = client.published
    assert published["topic"] == "wadiwatch/down"
    assert json.loads(published["payload"]) == {
        "v": 1,
        "kind": "all_clear",
        "command": response.json()["command"],
        "ts": int(clock()),
        "wadi": wadi.id,
        "auth": command_auth_code(
            TEST_COMMAND_KEY, response.json()["command"], int(clock()), wadi.id
        ),
    }
