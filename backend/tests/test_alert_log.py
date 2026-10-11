"""Gateway messages in, the alert log out. Nobody signs in here: the log is public."""

import json

from conftest import gateway_output
from wadiwatch_backend.replay import DEFAULT_SCENARIO, load_scenario, replay


def summary(entries):
    """(type, node, alert level) of each entry, enough to tell them apart."""
    return [(entry["type"], entry["node"], entry.get("alert_level")) for entry in entries]


def siren_acknowledgements(entry):
    """Siren node identifier -> time it acknowledged this alert, or None."""
    return {siren["id"]: siren["acknowledged_ts"] for siren in entry["siren_nodes"]}


def test_the_alert_log_is_empty_before_any_message(visitor):
    assert visitor.alert_log() == []


def test_alerts_tests_and_all_clears_are_distinct_entries_readable_without_signing_in(
    harness, visitor, site, clock, upstream, siren
):
    harness.ingest("test", siren.id)
    test_ts = int(clock())
    clock.advance(600)
    harness.ingest("alert", upstream.id, alert_level="warning")
    alert_ts = int(clock())
    clock.advance(3600)
    harness.ingest("all_clear", site.gateway.id, wadi_node=upstream.id)
    all_clear_ts = int(clock())

    assert visitor.session() == {"signed_in": False}
    all_clear, alert, test = visitor.alert_log()

    assert (test["type"], test["node"], test["ts"]) == ("test", siren.id, test_ts)
    assert test["node_name"] == siren.name
    assert (alert["type"], alert["node"], alert["ts"]) == ("alert", upstream.id, alert_ts)
    assert alert["node_name"] == upstream.name
    assert alert["alert_level"] == "warning"
    assert (all_clear["type"], all_clear["ts"]) == ("all_clear", all_clear_ts)
    assert all_clear["wadi_node"] == upstream.id
    assert all_clear["wadi_node_name"] == upstream.name
    assert len({test["id"], alert["id"], all_clear["id"]}) == 3


def test_readings_heartbeats_and_acknowledgements_are_not_entries(
    harness, visitor, upstream, siren
):
    harness.ingest("reading", upstream.id, level_cm=120.0, rise_cm_min=6.0)
    harness.ingest(
        "heartbeat", upstream.id, battery_mv=3900, rssi_dbm=-90, low_battery=False, tamper=False
    )
    harness.ingest("acknowledgement", siren.id, wadi_node=upstream.id, alert_level="warning")

    assert visitor.alert_log() == []


def test_each_change_of_alert_level_is_one_entry_and_a_refresh_adds_none(
    harness, visitor, clock, upstream
):
    harness.ingest("alert", upstream.id, alert_level="watch")
    clock.advance(300)
    harness.ingest("alert", upstream.id, alert_level="warning")
    warning_ts = int(clock())
    # At Warning and above the wadi node repeats its alert every minute.
    for _ in range(5):
        clock.advance(60)
        harness.ingest("alert", upstream.id, alert_level="warning")

    entries = visitor.alert_log()

    assert summary(entries) == [
        ("alert", upstream.id, "warning"),
        ("alert", upstream.id, "watch"),
    ]
    assert entries[0]["ts"] == warning_ts


def test_a_wadi_node_stating_no_alert_is_not_an_entry(harness, visitor, upstream):
    harness.ingest("alert", upstream.id, alert_level="none")

    assert visitor.alert_log() == []


def test_alerts_of_different_wadi_nodes_are_separate_entries_newest_first(
    harness, visitor, clock, upstream, downstream
):
    harness.ingest("alert", upstream.id, alert_level="warning")
    clock.advance(120)
    harness.ingest("alert", downstream.id, alert_level="warning")
    clock.advance(60)
    harness.ingest("alert", upstream.id, alert_level="warning")
    harness.ingest("alert", downstream.id, alert_level="evacuate")

    assert summary(visitor.alert_log()) == [
        ("alert", downstream.id, "evacuate"),
        ("alert", downstream.id, "warning"),
        ("alert", upstream.id, "warning"),
    ]


def test_only_the_latest_alert_of_a_wadi_node_is_in_force_until_the_all_clear(
    harness, visitor, site, upstream
):
    harness.ingest("alert", upstream.id, alert_level="watch")
    harness.ingest("alert", upstream.id, alert_level="warning")

    warning, watch = visitor.alerts()
    assert (warning["in_force"], watch["in_force"]) == (True, False)

    harness.ingest("all_clear", site.gateway.id, wadi_node=upstream.id)

    assert [alert["in_force"] for alert in visitor.alerts()] == [False, False]


def test_an_alert_after_an_all_clear_is_a_new_entry_even_at_the_same_level(
    harness, visitor, site, clock, upstream
):
    harness.ingest("alert", upstream.id, alert_level="warning")
    harness.ingest("all_clear", site.gateway.id, wadi_node=upstream.id)
    clock.advance(86400)
    harness.ingest("alert", upstream.id, alert_level="warning")

    assert summary(visitor.alert_log()) == [
        ("alert", upstream.id, "warning"),
        ("all_clear", site.gateway.id, None),
        ("alert", upstream.id, "warning"),
    ]


# Which siren nodes acknowledged each alert


def test_an_alert_lists_every_siren_node_as_not_acknowledged_until_it_answers(
    harness, visitor, site, upstream
):
    harness.ingest("alert", upstream.id, alert_level="warning")

    (alert,) = visitor.alerts()

    assert [siren["id"] for siren in alert["siren_nodes"]] == [
        node.id for node in site.siren_nodes
    ]
    assert [siren["name"] for siren in alert["siren_nodes"]] == [
        node.name for node in site.siren_nodes
    ]
    assert set(siren_acknowledgements(alert).values()) == {None}


def test_a_siren_node_acknowledgement_is_recorded_against_the_alert_it_answers(
    harness, visitor, clock, upstream, siren
):
    warning = harness.message("alert", upstream.id, alert_level="warning")
    harness.ingest_raw(json.dumps(warning))
    clock.advance(60)
    evacuate = harness.message("alert", upstream.id, alert_level="evacuate")
    harness.ingest_raw(json.dumps(evacuate))
    clock.advance(2)
    # The siren node's answer to the Warning arrives after the node has moved on to Evacuate.
    harness.ingest(
        "acknowledgement",
        siren.id,
        wadi_node=upstream.id,
        alert_level="warning",
        alert_counter=warning["counter"],
    )
    acknowledged_ts = int(clock())

    evacuate_entry, warning_entry = visitor.alerts()

    assert siren_acknowledgements(warning_entry)[siren.id] == acknowledged_ts
    assert siren_acknowledgements(evacuate_entry)[siren.id] is None


def test_an_acknowledgement_of_a_refresh_counts_for_the_alert_being_refreshed(
    harness, visitor, clock, upstream, siren
):
    harness.ingest("alert", upstream.id, alert_level="warning")
    clock.advance(60)
    refresh = harness.message("alert", upstream.id, alert_level="warning")
    harness.ingest_raw(json.dumps(refresh))
    clock.advance(2)
    harness.ingest(
        "acknowledgement",
        siren.id,
        wadi_node=upstream.id,
        alert_level="warning",
        alert_counter=refresh["counter"],
    )

    (alert,) = visitor.alerts()

    assert siren_acknowledgements(alert)[siren.id] == int(clock())


def test_the_first_acknowledgement_time_is_kept_when_a_siren_node_answers_every_refresh(
    harness, visitor, clock, upstream, siren
):
    harness.ingest("alert", upstream.id, alert_level="warning")
    clock.advance(2)
    harness.ingest("acknowledgement", siren.id, wadi_node=upstream.id, alert_level="warning")
    first_ts = int(clock())
    for _ in range(3):
        clock.advance(60)
        harness.ingest("alert", upstream.id, alert_level="warning")
        harness.ingest("acknowledgement", siren.id, wadi_node=upstream.id, alert_level="warning")

    (alert,) = visitor.alerts()

    assert siren_acknowledgements(alert)[siren.id] == first_ts


def test_an_acknowledgement_without_an_alert_counter_goes_to_the_current_alert_of_that_level(
    harness, visitor, clock, upstream, siren
):
    harness.ingest("alert", upstream.id, alert_level="watch")
    harness.ingest("alert", upstream.id, alert_level="warning")
    clock.advance(2)
    harness.ingest("acknowledgement", siren.id, wadi_node=upstream.id, alert_level="warning")

    warning, watch = visitor.alerts()

    assert siren_acknowledgements(warning)[siren.id] == int(clock())
    assert siren_acknowledgements(watch)[siren.id] is None


def test_an_acknowledgement_for_one_wadi_node_does_not_count_for_another(
    harness, visitor, upstream, downstream, siren
):
    harness.ingest("alert", upstream.id, alert_level="warning")
    harness.ingest("alert", downstream.id, alert_level="warning")
    harness.ingest("acknowledgement", siren.id, wadi_node=downstream.id, alert_level="warning")

    downstream_alert, upstream_alert = visitor.alerts()

    assert siren_acknowledgements(downstream_alert)[siren.id] is not None
    assert siren_acknowledgements(upstream_alert)[siren.id] is None


def test_an_acknowledgement_from_before_the_all_clear_does_not_count_for_the_next_alert(
    harness, visitor, site, clock, upstream, siren
):
    harness.ingest("alert", upstream.id, alert_level="warning")
    harness.ingest("acknowledgement", siren.id, wadi_node=upstream.id, alert_level="warning")
    harness.ingest("all_clear", site.gateway.id, wadi_node=upstream.id)
    clock.advance(86400)
    harness.ingest("alert", upstream.id, alert_level="warning")

    new_alert, old_alert = visitor.alerts()

    assert siren_acknowledgements(new_alert)[siren.id] is None
    assert siren_acknowledgements(old_alert)[siren.id] is not None


def test_the_alert_log_survives_a_restart(harness, visitor, site, upstream, siren):
    harness.ingest("test", siren.id)
    harness.ingest("alert", upstream.id, alert_level="warning")
    harness.ingest("acknowledgement", siren.id, wadi_node=upstream.id, alert_level="warning")
    harness.ingest("all_clear", site.gateway.id, wadi_node=upstream.id)
    before = visitor.alert_log()

    harness.restart()

    assert len(before) == 3
    assert visitor.alert_log() == before


def test_the_flood_scenario_leaves_a_reviewable_alert_log(
    harness, ingest_over_http, visitor, site, clock, upstream, downstream, siren
):
    def send(payload):
        return ingest_over_http(payload).json()["status"]

    replay(load_scenario(DEFAULT_SCENARIO), send, instant=True, clock=clock)

    entries = visitor.alert_log()
    oldest_first = summary(reversed(entries))
    assert oldest_first == [
        ("test", siren.id, None),
        ("alert", upstream.id, "watch"),
        ("alert", upstream.id, "warning"),
        ("alert", downstream.id, "watch"),
        ("alert", downstream.id, "warning"),
        ("alert", downstream.id, "evacuate"),
        ("all_clear", site.gateway.id, None),
        ("all_clear", site.gateway.id, None),
    ]
    for entry in entries:
        if entry["type"] == "alert":
            sounded = entry["alert_level"] in ("warning", "evacuate")
            assert (siren_acknowledgements(entry)[siren.id] is not None) == sounded
            if sounded:
                assert 0 <= siren_acknowledgements(entry)[siren.id] - entry["ts"] <= 5


# The map and the alert log each work out which alerts are in force, in their own way:
# the map from each wadi node's latest alert and all clear, the log by reading every
# message in order. They must never disagree.


def assert_map_and_log_agree(harness, visitor, site):
    in_force = {
        entry["node"]: entry["alert_level"] for entry in visitor.alerts() if entry["in_force"]
    }
    nodes = harness.nodes()
    for node in site.wadi_nodes:
        assert nodes[node.id]["alert_level"] == in_force.get(node.id, "none"), node.name


def test_the_map_and_the_alert_log_agree_on_the_alerts_in_force_through_a_whole_event(
    harness, visitor, site
):
    for message in load_scenario(DEFAULT_SCENARIO):
        if harness.ingest_raw(json.dumps(message)).status != "stored":
            continue
        if message["kind"] in ("alert", "all_clear"):
            assert_map_and_log_agree(harness, visitor, site)
    assert_map_and_log_agree(harness, visitor, site)


def test_the_map_and_the_alert_log_agree_on_what_the_gateway_core_produces(harness, visitor, site):
    for raw in gateway_output():
        assert harness.ingest_raw(raw).status == "stored"
        assert_map_and_log_agree(harness, visitor, site)


def test_the_map_and_the_alert_log_agree_when_a_node_states_no_alert_or_is_cleared_twice(
    harness, visitor, site, upstream, downstream
):
    steps = [
        ("alert", upstream.id, {"alert_level": "warning"}),
        ("alert", downstream.id, {"alert_level": "watch"}),
        ("alert", upstream.id, {"alert_level": "none"}),
        ("alert", upstream.id, {"alert_level": "evacuate"}),
        ("all_clear", site.gateway.id, {"wadi_node": upstream.id}),
        ("all_clear", site.gateway.id, {"wadi_node": upstream.id}),
        ("alert", upstream.id, {"alert_level": "evacuate"}),
        ("all_clear", site.gateway.id, {"wadi_node": downstream.id}),
    ]
    for kind, node, fields in steps:
        assert harness.ingest(kind, node, **fields).status == "stored"
        assert_map_and_log_agree(harness, visitor, site)
