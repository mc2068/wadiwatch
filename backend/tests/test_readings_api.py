"""Readings in, the curve of a wadi node out: level and rate of rise over a chosen period."""

import json

MINUTE = 60
HOUR = 3600


def curve(harness, node, **period):
    """The readings API for one node. `period` may give `start` and `end`, in Unix seconds."""
    query = "&".join(
        f"{name}={period[key]}" for name, key in (("from", "start"), ("to", "end")) if key in period
    )
    return harness.api(f"/api/nodes/{node.id}/readings" + (f"?{query}" if query else ""))


def test_curve_is_the_level_and_rate_of_rise_of_each_reading_in_time_order(
    harness, clock, upstream
):
    start = int(clock())
    harness.ingest("reading", upstream.id, level_cm=20.0, rise_cm_min=0.0)
    clock.advance(MINUTE)
    harness.ingest("reading", upstream.id, level_cm=26.5, rise_cm_min=6.5)
    clock.advance(MINUTE)
    harness.ingest("reading", upstream.id, level_cm=24.0, rise_cm_min=-2.5)

    series = curve(harness, upstream, start=start, end=int(clock()))

    assert series["node"] == upstream.id
    assert series["readings"] == [
        {"ts": start, "level_cm": 20.0, "rise_cm_min": 0.0},
        {"ts": start + MINUTE, "level_cm": 26.5, "rise_cm_min": 6.5},
        {"ts": start + 2 * MINUTE, "level_cm": 24.0, "rise_cm_min": -2.5},
    ]


def test_curve_covers_the_chosen_period_and_nothing_outside_it(harness, clock, upstream):
    start = int(clock())
    for minute in range(10):
        harness.ingest("reading", upstream.id, level_cm=10.0 + minute, rise_cm_min=1.0)
        clock.advance(MINUTE)

    series = curve(harness, upstream, start=start + 3 * MINUTE, end=start + 6 * MINUTE)

    # Both ends of the period are included.
    assert [reading["level_cm"] for reading in series["readings"]] == [13.0, 14.0, 15.0, 16.0]
    assert (series["from"], series["to"]) == (start + 3 * MINUTE, start + 6 * MINUTE)


def test_curve_of_one_wadi_node_leaves_out_the_other(harness, clock, upstream, downstream):
    harness.ingest("reading", upstream.id, level_cm=40.0, rise_cm_min=2.0)
    harness.ingest("reading", downstream.id, level_cm=15.0, rise_cm_min=0.5)

    assert [r["level_cm"] for r in curve(harness, upstream)["readings"]] == [40.0]
    assert [r["level_cm"] for r in curve(harness, downstream)["readings"]] == [15.0]


def test_only_readings_make_the_curve(harness, clock, upstream):
    harness.ingest("reading", upstream.id, level_cm=70.0, rise_cm_min=3.0)
    clock.advance(MINUTE)
    harness.ingest("alert", upstream.id, alert_level="watch")
    # A heartbeat repeats the last level measured. It is not a measurement.
    harness.ingest(
        "heartbeat",
        upstream.id,
        battery_mv=3900,
        rssi_dbm=-90,
        low_battery=False,
        tamper=False,
        level_cm=70.0,
    )

    assert len(curve(harness, upstream)["readings"]) == 1


def test_without_a_period_the_curve_ends_now_and_covers_the_last_day(harness, clock, upstream):
    harness.ingest("reading", upstream.id, level_cm=5.0, rise_cm_min=0.0)
    clock.advance(25 * HOUR)
    harness.ingest("reading", upstream.id, level_cm=6.0, rise_cm_min=0.0)
    clock.advance(HOUR)

    series = curve(harness, upstream)

    assert series["to"] == int(clock())
    assert series["from"] == int(clock()) - 24 * HOUR
    assert [reading["level_cm"] for reading in series["readings"]] == [6.0]


def test_a_period_may_be_given_by_its_start_alone(harness, clock, upstream):
    harness.ingest("reading", upstream.id, level_cm=5.0, rise_cm_min=0.0)
    clock.advance(2 * HOUR)
    harness.ingest("reading", upstream.id, level_cm=6.0, rise_cm_min=0.0)

    series = curve(harness, upstream, start=int(clock()) - HOUR)

    assert (series["from"], series["to"]) == (int(clock()) - HOUR, int(clock()))
    assert [reading["level_cm"] for reading in series["readings"]] == [6.0]


def test_readings_forwarded_after_an_uplink_outage_appear_at_the_time_they_were_measured(
    harness, clock, upstream
):
    measured_at = int(clock())
    held_back = [
        harness.message("reading", upstream.id, level_cm=30.0 + minute, rise_cm_min=1.0)
        for minute in range(3)
    ]
    for minute, message in enumerate(held_back):
        message["ts"] = measured_at + minute * MINUTE
    # The uplink returns two hours later and the gateway forwards what it stored, in order.
    clock.advance(2 * HOUR)
    for message in held_back:
        assert harness.ingest_raw(json.dumps(message)).status == "stored"
    harness.ingest("reading", upstream.id, level_cm=12.0, rise_cm_min=-0.5)

    series = curve(harness, upstream, start=measured_at, end=int(clock()))

    assert [(reading["ts"] - measured_at, reading["level_cm"]) for reading in series["readings"]] == [
        (0, 30.0),
        (MINUTE, 31.0),
        (2 * MINUTE, 32.0),
        (2 * HOUR, 12.0),
    ]


def test_curve_is_empty_before_any_reading(harness, upstream):
    assert curve(harness, upstream)["readings"] == []


def test_curve_survives_a_restart(harness, upstream):
    harness.ingest("reading", upstream.id, level_cm=88.0, rise_cm_min=4.0)
    before = curve(harness, upstream)

    harness.restart()

    assert curve(harness, upstream) == before
    assert before["readings"][0]["level_cm"] == 88.0


def test_only_a_wadi_node_has_a_curve(harness, site, siren):
    unknown = max(node.id for node in site.nodes) + site.gateway.id

    for node_id in (siren.id, site.gateway.id, unknown):
        response = harness.client.get(f"/api/nodes/{node_id}/readings")
        assert response.status_code == 404, node_id


def test_a_period_that_ends_before_it_starts_is_refused(harness, clock, upstream):
    now = int(clock())

    response = harness.client.get(f"/api/nodes/{upstream.id}/readings?from={now}&to={now - 1}")

    assert response.status_code == 400
