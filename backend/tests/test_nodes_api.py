"""Gateway messages in, the nodes API out."""

import json

from wadiwatch_backend.contract import MAX_UINT32


def test_every_configured_node_is_listed_before_any_message(harness, site):
    nodes = harness.nodes()

    assert set(nodes) == {node.id for node in site.nodes}
    for configured in site.nodes:
        node = nodes[configured.id]
        assert node["name"] == configured.name
        assert node["role"] == configured.role
        assert (node["lat"], node["lon"]) == (configured.lat, configured.lon)
        assert node["alert_level"] == "none"
        assert node["level_cm"] is None
        assert node["rise_cm_min"] is None
        assert node["last_message_ts"] is None


def test_latest_reading_of_each_node_is_returned(harness, clock, upstream, downstream):
    harness.ingest("reading", upstream.id, level_cm=20.0, rise_cm_min=0.0)
    clock.advance(60)
    harness.ingest("reading", downstream.id, level_cm=12.5, rise_cm_min=0.5)
    clock.advance(60)
    harness.ingest("reading", upstream.id, level_cm=26.5, rise_cm_min=3.25)
    latest_upstream_ts = int(clock())

    nodes = harness.nodes()

    assert nodes[upstream.id]["level_cm"] == 26.5
    assert nodes[upstream.id]["rise_cm_min"] == 3.25
    assert nodes[upstream.id]["level_ts"] == latest_upstream_ts
    assert nodes[downstream.id]["level_cm"] == 12.5
    assert nodes[downstream.id]["rise_cm_min"] == 0.5
    assert nodes[downstream.id]["level_ts"] == latest_upstream_ts - 60


def test_readings_alone_leave_the_alert_level_at_none(harness, upstream):
    harness.ingest("reading", upstream.id, level_cm=500.0, rise_cm_min=20.0)

    assert harness.nodes()[upstream.id]["alert_level"] == "none"


def test_the_alert_level_carried_in_a_reading_does_not_change_the_alert_level(harness, upstream):
    harness.ingest(
        "reading",
        upstream.id,
        level_cm=160.0,
        rise_cm_min=6.0,
        rain_mm=14.2,
        storm_mode=True,
        alert_level="evacuate",
    )

    node = harness.nodes()[upstream.id]
    assert node["level_cm"] == 160.0
    assert node["alert_level"] == "none"


def test_an_alert_may_carry_the_level_that_raised_it(harness, upstream):
    result = harness.ingest(
        "alert", upstream.id, alert_level="warning", level_cm=104.0, rise_cm_min=5.1
    )

    assert result.status == "stored", result.error
    assert harness.nodes()[upstream.id]["alert_level"] == "warning"


def test_alert_changes_the_alert_level_of_that_node_only(harness, clock, upstream, downstream):
    harness.ingest("reading", upstream.id, level_cm=110.0, rise_cm_min=4.0)
    harness.ingest("reading", downstream.id, level_cm=30.0, rise_cm_min=1.0)

    harness.ingest("alert", upstream.id, alert_level="warning")
    alert_ts = int(clock())

    nodes = harness.nodes()
    assert nodes[upstream.id]["alert_level"] == "warning"
    assert nodes[upstream.id]["alert_ts"] == alert_ts
    assert nodes[upstream.id]["level_cm"] == 110.0
    assert nodes[downstream.id]["alert_level"] == "none"
    assert nodes[downstream.id]["alert_ts"] is None


def test_alert_level_follows_the_latest_alert(harness, clock, upstream):
    for level in ("watch", "warning", "evacuate"):
        harness.ingest("alert", upstream.id, alert_level=level)
        assert harness.nodes()[upstream.id]["alert_level"] == level
        clock.advance(60)

    # A refresh repeats the level every minute and changes nothing.
    harness.ingest("alert", upstream.id, alert_level="evacuate")
    assert harness.nodes()[upstream.id]["alert_level"] == "evacuate"


def test_siren_node_shows_the_alert_level_it_acknowledged(harness, upstream, siren):
    harness.ingest("alert", upstream.id, alert_level="warning")
    assert harness.nodes()[siren.id]["alert_level"] == "none"

    harness.ingest("acknowledgement", siren.id, wadi_node=upstream.id, alert_level="warning")

    assert harness.nodes()[siren.id]["alert_level"] == "warning"


def test_siren_node_shows_the_highest_level_among_the_wadi_nodes_it_answers(
    harness, clock, upstream, downstream, siren
):
    harness.ingest("acknowledgement", siren.id, wadi_node=downstream.id, alert_level="evacuate")
    clock.advance(10)
    # The upstream node's refresh at a lower level arrives later.
    harness.ingest("acknowledgement", siren.id, wadi_node=upstream.id, alert_level="warning")

    assert harness.nodes()[siren.id]["alert_level"] == "evacuate"


def test_all_clear_returns_the_wadi_node_and_its_siren_nodes_to_none(
    harness, site, clock, upstream, siren
):
    harness.ingest("alert", upstream.id, alert_level="evacuate")
    harness.ingest("acknowledgement", siren.id, wadi_node=upstream.id, alert_level="evacuate")
    clock.advance(3600)

    harness.ingest("all_clear", site.gateway.id, wadi_node=upstream.id)

    nodes = harness.nodes()
    assert nodes[upstream.id]["alert_level"] == "none"
    assert nodes[siren.id]["alert_level"] == "none"


def test_all_clear_for_one_wadi_node_leaves_the_other_alert_in_force(
    harness, site, upstream, downstream, siren
):
    harness.ingest("alert", upstream.id, alert_level="warning")
    harness.ingest("alert", downstream.id, alert_level="evacuate")
    harness.ingest("acknowledgement", siren.id, wadi_node=upstream.id, alert_level="warning")
    harness.ingest("acknowledgement", siren.id, wadi_node=downstream.id, alert_level="evacuate")

    harness.ingest("all_clear", site.gateway.id, wadi_node=upstream.id)

    nodes = harness.nodes()
    assert nodes[upstream.id]["alert_level"] == "none"
    assert nodes[downstream.id]["alert_level"] == "evacuate"
    assert nodes[siren.id]["alert_level"] == "evacuate"


def test_a_new_alert_after_an_all_clear_is_shown(harness, site, clock, upstream, siren):
    harness.ingest("alert", upstream.id, alert_level="evacuate")
    harness.ingest("acknowledgement", siren.id, wadi_node=upstream.id, alert_level="evacuate")
    harness.ingest("all_clear", site.gateway.id, wadi_node=upstream.id)
    clock.advance(86400)

    harness.ingest("alert", upstream.id, alert_level="watch")

    nodes = harness.nodes()
    assert nodes[upstream.id]["alert_level"] == "watch"
    assert nodes[siren.id]["alert_level"] == "none"


def test_last_message_time_covers_every_kind(harness, clock, upstream, siren):
    harness.ingest("reading", upstream.id, level_cm=10.0, rise_cm_min=0.0)
    clock.advance(600)
    harness.ingest(
        "heartbeat", upstream.id, battery_mv=3900, rssi_dbm=-90, low_battery=False, tamper=False
    )
    heartbeat_ts = int(clock())
    harness.ingest("test", siren.id)

    nodes = harness.nodes()
    assert nodes[upstream.id]["last_message_ts"] == heartbeat_ts
    assert nodes[upstream.id]["level_ts"] == heartbeat_ts - 600
    assert nodes[siren.id]["last_message_ts"] == heartbeat_ts


def test_site_and_gateway_are_described_for_the_map(harness, site):
    described = harness.api("/api/nodes")

    assert described["site"]["name"] == site.name
    assert described["site"]["positions_are_placeholders"] is True
    assert described["gateway"]["id"] == site.gateway.id
    assert (described["gateway"]["lat"], described["gateway"]["lon"]) == (
        site.gateway.lat,
        site.gateway.lon,
    )


def test_stored_messages_survive_a_restart(harness, upstream, siren):
    harness.ingest("reading", upstream.id, level_cm=142.0, rise_cm_min=6.5)
    harness.ingest("alert", upstream.id, alert_level="warning")
    harness.ingest("acknowledgement", siren.id, wadi_node=upstream.id, alert_level="warning")
    before = harness.nodes()

    harness.restart()

    assert harness.nodes() == before
    assert harness.nodes()[upstream.id]["level_cm"] == 142.0
    assert harness.nodes()[upstream.id]["alert_level"] == "warning"


def test_a_message_delivered_twice_is_stored_once(harness, clock, upstream):
    first = json.dumps(harness.message("alert", upstream.id, alert_level="watch"))
    assert harness.ingest_raw(first).status == "stored"
    clock.advance(60)
    harness.ingest("alert", upstream.id, alert_level="warning")

    # The broker redelivers the first message after the second one.
    redelivered = harness.ingest_raw(first)

    assert redelivered.status == "duplicate"
    assert harness.nodes()[upstream.id]["alert_level"] == "warning"


def test_the_largest_packet_counter_is_accepted(harness, upstream):
    message = harness.message("reading", upstream.id, level_cm=1.0, rise_cm_min=0.0)
    message["counter"] = MAX_UINT32

    assert harness.ingest_raw(json.dumps(message)).status == "stored"
    assert harness.nodes()[upstream.id]["level_cm"] == 1.0
