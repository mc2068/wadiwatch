"""Heartbeats in, node health out; and how old the latest data from the gateway is.

The intervals come from the site file. "Two intervals" is the rule of the
specification: a node is faulty once its heartbeat has been missing for more
than two heartbeat intervals.
"""

import json

import pytest

TWO_INTERVALS = 2


def heartbeat(harness, node, **fields):
    """Ingest a heartbeat of a healthy node, with any field overridden."""
    values = {"battery_mv": 3900, "rssi_dbm": -90, "low_battery": False, "tamper": False}
    values.update(fields)
    return harness.ingest("heartbeat", node.id, **values)


def health(harness):
    """The health API, with its nodes keyed by node identifier."""
    described = harness.api("/api/health")
    described["nodes"] = {node["id"]: node for node in described["nodes"]}
    return described


def node_health(harness, node):
    return health(harness)["nodes"][node.id]


def gateway(harness):
    return health(harness)["gateway"]


# Node health


def test_every_configured_node_has_a_health_entry_before_any_heartbeat(harness, site):
    nodes = health(harness)["nodes"]

    assert set(nodes) == {node.id for node in site.nodes}
    for configured in site.nodes:
        node = nodes[configured.id]
        assert node["name"] == configured.name
        assert node["role"] == configured.role
        assert node["heartbeat_ts"] is None
        assert node["heartbeat_age_s"] is None
        assert node["battery_mv"] is None
        assert node["rssi_dbm"] is None
        assert node["low_battery"] is None
        assert node["tamper"] is None
        assert node["discards"] is None
        assert node["faulty"] is False


def test_health_shows_what_the_latest_heartbeat_reported(harness, clock, upstream):
    heartbeat(harness, upstream, battery_mv=4010, rssi_dbm=-71, discards=0)
    clock.advance(600)
    heartbeat(harness, upstream, battery_mv=3920, rssi_dbm=-97, discards=2)
    heartbeat_ts = int(clock())
    clock.advance(45)

    node = node_health(harness, upstream)

    assert node["battery_mv"] == 3920
    assert node["rssi_dbm"] == -97
    assert node["discards"] == 2
    assert node["low_battery"] is False
    assert node["tamper"] is False
    assert node["heartbeat_ts"] == heartbeat_ts
    assert node["heartbeat_age_s"] == 45


def test_health_of_one_node_says_nothing_about_another(harness, upstream, downstream, siren):
    heartbeat(harness, upstream, battery_mv=3700)
    heartbeat(harness, siren, battery_mv=12400, rssi_dbm=-76)

    nodes = health(harness)["nodes"]

    assert nodes[upstream.id]["battery_mv"] == 3700
    assert nodes[siren.id]["battery_mv"] == 12400
    assert nodes[siren.id]["rssi_dbm"] == -76
    assert nodes[downstream.id]["battery_mv"] is None


def test_low_battery_flag_of_a_heartbeat_appears_in_the_health_of_that_node(
    harness, upstream, downstream
):
    heartbeat(harness, upstream, battery_mv=3310, low_battery=True)
    heartbeat(harness, downstream)

    nodes = health(harness)["nodes"]

    assert nodes[upstream.id]["low_battery"] is True
    assert nodes[upstream.id]["tamper"] is False
    assert nodes[downstream.id]["low_battery"] is False


def test_tamper_flag_of_a_heartbeat_appears_in_the_health_of_that_node(harness, upstream, siren):
    heartbeat(harness, siren, tamper=True)
    heartbeat(harness, upstream)

    nodes = health(harness)["nodes"]

    assert nodes[siren.id]["tamper"] is True
    assert nodes[siren.id]["low_battery"] is False
    assert nodes[upstream.id]["tamper"] is False


def test_flags_are_cleared_by_a_later_heartbeat_without_them(harness, clock, upstream):
    heartbeat(harness, upstream, low_battery=True, tamper=True)
    assert node_health(harness, upstream)["low_battery"] is True
    assert node_health(harness, upstream)["tamper"] is True
    clock.advance(3600)

    heartbeat(harness, upstream, low_battery=False, tamper=False)

    assert node_health(harness, upstream)["low_battery"] is False
    assert node_health(harness, upstream)["tamper"] is False


def test_siren_node_that_lost_its_alert_refreshes_says_so_in_its_health(harness, siren, upstream):
    heartbeat(harness, siren, refresh_lost=True)
    heartbeat(harness, upstream)

    nodes = health(harness)["nodes"]

    assert nodes[siren.id]["refresh_lost"] is True
    assert nodes[upstream.id]["refresh_lost"] is None


def test_health_survives_a_restart(harness, upstream):
    heartbeat(harness, upstream, battery_mv=3555, low_battery=True, discards=7)
    before = node_health(harness, upstream)

    harness.restart()

    assert node_health(harness, upstream) == before
    assert before["battery_mv"] == 3555


# Missing heartbeat


def test_node_is_faulty_once_its_heartbeat_is_missing_for_more_than_two_intervals(
    harness, site, clock, upstream
):
    heartbeat(harness, upstream)
    assert node_health(harness, upstream)["faulty"] is False

    clock.advance(TWO_INTERVALS * site.heartbeat_interval_s)
    assert node_health(harness, upstream)["faulty"] is False

    clock.advance(1)
    assert node_health(harness, upstream)["faulty"] is True


def test_faulty_node_recovers_when_a_heartbeat_arrives(harness, site, clock, upstream):
    heartbeat(harness, upstream)
    clock.advance(TWO_INTERVALS * site.heartbeat_interval_s + 1)
    assert node_health(harness, upstream)["faulty"] is True

    heartbeat(harness, upstream)

    assert node_health(harness, upstream)["faulty"] is False
    assert node_health(harness, upstream)["heartbeat_age_s"] == 0


def test_only_the_node_whose_heartbeat_is_missing_is_faulty(
    harness, site, clock, upstream, downstream, siren
):
    for node in (upstream, downstream, siren):
        heartbeat(harness, node)
    for _ in range(TWO_INTERVALS + 1):
        clock.advance(site.heartbeat_interval_s)
        heartbeat(harness, upstream)
        heartbeat(harness, siren)

    nodes = health(harness)["nodes"]

    assert nodes[downstream.id]["faulty"] is True
    assert nodes[upstream.id]["faulty"] is False
    assert nodes[siren.id]["faulty"] is False


def test_faulty_node_still_shows_what_its_last_heartbeat_reported(harness, site, clock, upstream):
    heartbeat(harness, upstream, battery_mv=3400, low_battery=True)
    silence = TWO_INTERVALS * site.heartbeat_interval_s + 600
    clock.advance(silence)

    node = node_health(harness, upstream)

    assert node["faulty"] is True
    assert node["battery_mv"] == 3400
    assert node["low_battery"] is True
    assert node["heartbeat_age_s"] == silence


def test_readings_do_not_stand_in_for_a_missing_heartbeat(harness, site, clock, upstream):
    heartbeat(harness, upstream)
    clock.advance(TWO_INTERVALS * site.heartbeat_interval_s + 1)

    harness.ingest("reading", upstream.id, level_cm=12.0, rise_cm_min=0.0)

    assert node_health(harness, upstream)["faulty"] is True


def test_node_that_never_sent_a_heartbeat_is_faulty_once_the_site_was_heard_for_two_intervals(
    harness, site, clock, upstream, siren
):
    heartbeat(harness, upstream)
    clock.advance(site.heartbeat_interval_s)
    heartbeat(harness, upstream)
    clock.advance(site.heartbeat_interval_s)
    heartbeat(harness, upstream)
    # Two intervals of traffic from the site and the siren node has not been heard: not yet a fault.
    assert node_health(harness, siren)["faulty"] is False

    clock.advance(1)

    assert node_health(harness, siren)["faulty"] is True
    assert node_health(harness, siren)["heartbeat_ts"] is None
    assert node_health(harness, upstream)["faulty"] is False


def test_heartbeat_age_counts_from_the_time_the_gateway_heard_it(harness, site, clock, upstream):
    held_back = harness.message(
        "heartbeat", upstream.id, battery_mv=3900, rssi_dbm=-90, low_battery=False, tamper=False
    )
    heard_at = held_back["ts"]
    # The uplink is down for longer than two intervals; the gateway then forwards what it stored.
    outage = TWO_INTERVALS * site.heartbeat_interval_s + 900
    clock.advance(outage)
    harness.ingest_raw(json.dumps(held_back))

    node = node_health(harness, upstream)

    assert node["heartbeat_ts"] == heard_at
    assert node["heartbeat_age_s"] == outage
    assert node["faulty"] is True


def test_health_states_the_rule_it_applies(harness, site):
    described = health(harness)

    assert described["heartbeat_interval_s"] == site.heartbeat_interval_s
    assert described["faulty_after_s"] == TWO_INTERVALS * site.heartbeat_interval_s


# Age of the latest data from the gateway


def test_nothing_received_yet_is_reported_as_stale_with_no_age(harness, site):
    described = gateway(harness)

    assert described["id"] == site.gateway.id
    assert described["name"] == site.gateway.name
    assert described["last_received_at"] is None
    assert described["age_s"] is None
    assert described["stale"] is True


def test_age_of_the_latest_message_received_from_the_gateway(harness, clock, upstream):
    harness.ingest("reading", upstream.id, level_cm=10.0, rise_cm_min=0.0)
    received_at = int(clock())
    assert gateway(harness)["age_s"] == 0

    clock.advance(42)

    described = gateway(harness)
    assert described["age_s"] == 42
    assert described["last_received_at"] == received_at
    assert health(harness)["now"] == received_at + 42


@pytest.mark.parametrize("kind", ["reading", "alert", "heartbeat", "acknowledgement", "test", "all_clear"])
def test_a_message_of_any_kind_is_data_from_the_gateway(
    harness, site, clock, upstream, siren, kind
):
    harness.ingest("reading", upstream.id, level_cm=10.0, rise_cm_min=0.0)
    clock.advance(500)

    if kind == "reading":
        harness.ingest(kind, upstream.id, level_cm=11.0, rise_cm_min=0.1)
    elif kind == "alert":
        harness.ingest(kind, upstream.id, alert_level="watch")
    elif kind == "heartbeat":
        heartbeat(harness, siren)
    elif kind == "acknowledgement":
        harness.ingest(kind, siren.id, wadi_node=upstream.id, alert_level="warning")
    elif kind == "test":
        harness.ingest(kind, siren.id)
    else:
        harness.ingest(kind, site.gateway.id, wadi_node=upstream.id)
    clock.advance(7)

    assert gateway(harness)["age_s"] == 7


def test_age_counts_from_arrival_not_from_the_time_stamped_by_the_gateway(
    harness, clock, upstream
):
    held_back = harness.message("reading", upstream.id, level_cm=55.0, rise_cm_min=2.0)
    # Forwarded three hours late, after an uplink outage: the gateway is back, the data is old.
    clock.advance(3 * 3600)
    harness.ingest_raw(json.dumps(held_back))
    clock.advance(5)

    assert gateway(harness)["age_s"] == 5
    assert gateway(harness)["stale"] is False


def test_a_rejected_message_is_not_data_from_the_gateway(harness, clock, upstream):
    harness.ingest("reading", upstream.id, level_cm=10.0, rise_cm_min=0.0)
    clock.advance(300)

    assert harness.ingest_raw('{"kind": "reading"').status == "rejected"

    assert gateway(harness)["age_s"] == 300


def test_age_keeps_counting_across_a_restart(harness, clock, upstream):
    harness.ingest("reading", upstream.id, level_cm=10.0, rise_cm_min=0.0)
    clock.advance(120)

    harness.restart()
    clock.advance(30)

    assert gateway(harness)["age_s"] == 150


# Stale data


def test_data_is_stale_once_the_gateway_is_silent_for_more_than_two_dry_mode_intervals(
    harness, site, clock, upstream
):
    harness.ingest("reading", upstream.id, level_cm=10.0, rise_cm_min=0.0)
    assert gateway(harness)["stale"] is False

    clock.advance(TWO_INTERVALS * site.dry_mode_interval_s)
    assert gateway(harness)["stale"] is False

    clock.advance(1)
    assert gateway(harness)["stale"] is True
    assert gateway(harness)["stale_after_s"] == TWO_INTERVALS * site.dry_mode_interval_s


def test_stale_data_is_fresh_again_when_the_gateway_delivers_a_message(
    harness, site, clock, upstream
):
    harness.ingest("reading", upstream.id, level_cm=10.0, rise_cm_min=0.0)
    clock.advance(TWO_INTERVALS * site.dry_mode_interval_s + 1)
    assert gateway(harness)["stale"] is True

    harness.ingest("reading", upstream.id, level_cm=10.0, rise_cm_min=0.0)

    assert gateway(harness)["stale"] is False


@pytest.mark.parametrize("alert_level", ["warning", "evacuate"])
def test_during_a_warning_or_evacuate_two_missed_alert_refreshes_make_the_data_stale(
    harness, site, clock, upstream, downstream, alert_level
):
    harness.ingest("reading", upstream.id, level_cm=10.0, rise_cm_min=0.0)
    harness.ingest("alert", downstream.id, alert_level=alert_level)
    assert gateway(harness)["stale"] is False

    # A node at Warning or above repeats its alert every refresh interval.
    clock.advance(TWO_INTERVALS * site.alert_refresh_interval_s)
    assert gateway(harness)["stale"] is False

    clock.advance(1)
    assert gateway(harness)["stale"] is True
    assert gateway(harness)["stale_after_s"] == TWO_INTERVALS * site.alert_refresh_interval_s


def test_a_watch_does_not_shorten_the_silence_that_makes_data_stale(
    harness, site, clock, upstream
):
    # A node at Watch sends no refresh, and may be back in dry mode before the all clear.
    harness.ingest("alert", upstream.id, alert_level="watch")

    clock.advance(TWO_INTERVALS * site.alert_refresh_interval_s + 1)

    assert gateway(harness)["stale"] is False


def test_after_the_all_clear_the_dry_mode_silence_applies_again(harness, site, clock, upstream):
    harness.ingest("alert", upstream.id, alert_level="evacuate")
    clock.advance(site.alert_refresh_interval_s)
    harness.ingest("all_clear", site.gateway.id, wadi_node=upstream.id)

    clock.advance(TWO_INTERVALS * site.alert_refresh_interval_s + 1)

    assert gateway(harness)["stale"] is False
    assert gateway(harness)["stale_after_s"] == TWO_INTERVALS * site.dry_mode_interval_s
