"""The all clear: when the backend accepts it, who may issue it, and what it sends the gateway.

Gateway messages go in through the one ingest function; the officer's request
and everything that is checked afterwards go through the HTTP API, as the
dashboard does it. `harness.gateway` stands at the far end of the downlink: it
records the commands the backend hands over, and a test says what the gateway
does with one (`harness.gateway_relays`).
"""

import json

import pytest

from conftest import Harness
from officer_support import CSRF_HEADER, Browser
from wadiwatch_backend.backend import GatewayUnreachable
from wadiwatch_backend.downlink import all_clear_command
from wadiwatch_backend.simulated_gateway import SimulatedGateway
from wadiwatch_backend.site import DEFAULT_SITE_FILE, load_site

MINUTE = 60
# Clearly below and clearly above every Watch threshold of the prototype site.
LOW_CM = 12.0
HIGH_CM = 130.0


def stay(harness, clock, levels, seconds):
    """The water stays where it is for `seconds`: every wadi node in `levels` reports
    its level once a minute, and the last report is made now."""
    for _ in range(seconds // MINUTE):
        clock.advance(MINUTE)
        for node, level_cm in levels.items():
            harness.ingest("reading", node.id, level_cm=level_cm, rise_cm_min=0.0)


def flood(harness, clock, upstream, downstream, alert_level="warning"):
    """Both wadi nodes report a dry bed; then the upstream one floods and raises its alert."""
    for node in (upstream, downstream):
        harness.ingest("reading", node.id, level_cm=LOW_CM, rise_cm_min=0.0)
    clock.advance(MINUTE)
    harness.ingest("reading", upstream.id, level_cm=HIGH_CM, rise_cm_min=6.0)
    harness.ingest("alert", upstream.id, alert_level=alert_level, level_cm=HIGH_CM, rise_cm_min=6.0)
    harness.ingest("reading", downstream.id, level_cm=LOW_CM, rise_cm_min=0.0)


def fall(harness, clock, levels, quiet_for):
    """The water falls to these levels and has been there for `quiet_for` seconds:
    a first report at the new level, then one a minute, the last of them now."""
    stay(harness, clock, levels, MINUTE + quiet_for)


def flood_that_has_passed(
    harness, clock, site, upstream, downstream, quiet_for=None, alert_level="warning"
):
    """A flood at the upstream node. Both levels have been below the Watch threshold
    for `quiet_for` seconds: the quiet period unless said otherwise."""
    flood(harness, clock, upstream, downstream, alert_level)
    if quiet_for is None:
        quiet_for = site.all_clear_quiet_s
    fall(harness, clock, {upstream: LOW_CM, downstream: LOW_CM}, quiet_for)


def refusal_of(response, node):
    """Why a refused all clear names this wadi node, or None if it does not."""
    return next(judged for judged in response.json()["nodes"] if judged["id"] == node.id)["reason"]


def wadi_verdict(browser, wadi):
    response = browser.client.get("/api/wadis")
    assert response.status_code == 200, response.text
    return next(verdict for verdict in response.json()["wadis"] if verdict["wadi"]["id"] == wadi.id)


# Refused until the level has been below the Watch threshold for the quiet period


def test_an_all_clear_is_refused_while_the_level_is_above_the_watch_threshold(
    harness, signed_in, clock, wadi, upstream, downstream
):
    flood(harness, clock, upstream, downstream)
    stay(harness, clock, {upstream: HIGH_CM, downstream: LOW_CM}, 2 * 3600)

    response = signed_in.issue_all_clear(wadi.id)

    assert response.status_code == 409
    assert response.json()["allowed"] is False
    assert refusal_of(response, upstream) == "at_or_above_watch"
    assert refusal_of(response, downstream) is None
    assert upstream.name in response.json()["error"]
    # Nothing went to the gateway, and the alert is still in force.
    assert harness.gateway.commands == []
    assert signed_in.alerts()[0]["in_force"] is True
    assert harness.nodes()[upstream.id]["alert_level"] == "warning"


def test_a_level_exactly_at_the_watch_threshold_is_not_below_it(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    flood(harness, clock, upstream, downstream)
    stay(
        harness,
        clock,
        {upstream: upstream.watch_level_cm, downstream: LOW_CM},
        2 * site.all_clear_quiet_s,
    )

    response = signed_in.issue_all_clear(wadi.id)

    assert response.status_code == 409
    assert refusal_of(response, upstream) == "at_or_above_watch"


def test_an_all_clear_is_refused_when_the_level_has_been_below_the_threshold_for_less_than_the_quiet_period(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(
        harness, clock, site, upstream, downstream, quiet_for=site.all_clear_quiet_s - MINUTE
    )

    response = signed_in.issue_all_clear(wadi.id)

    assert response.status_code == 409
    assert refusal_of(response, upstream) == "below_watch_too_recently"
    judged = next(node for node in response.json()["nodes"] if node["id"] == upstream.id)
    # It says since when the level has been below, and from when that is long enough.
    assert judged["allowed_from"] == judged["below_since"] + site.all_clear_quiet_s
    assert judged["allowed_from"] == int(clock()) + MINUTE
    assert harness.gateway.commands == []


def test_an_all_clear_is_accepted_once_the_level_has_been_below_the_threshold_for_the_quiet_period(
    harness, signed_in, officer, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(harness, clock, site, upstream, downstream)

    response = signed_in.issue_all_clear(wadi.id)

    assert response.status_code == 200
    command = response.json()
    assert command == {
        "command": command["command"],
        "wadi": wadi.id,
        "officer": officer.name,
        "ts": int(clock()),
    }
    # The gateway is sent exactly the command the contract defines, naming the wadi.
    assert harness.gateway.raw == [all_clear_command(command["command"], int(clock()), wadi.id)]


def test_the_quiet_period_is_the_one_in_the_site_file(tmp_path, clock, upstream, downstream):
    raw = json.loads(DEFAULT_SITE_FILE.read_text(encoding="utf-8"))
    raw["all_clear_quiet_s"] = 10 * MINUTE
    path = tmp_path / "site.json"
    path.write_text(json.dumps(raw), encoding="utf-8")
    harness = Harness(tmp_path / "wadiwatch.db", load_site(path), clock)
    try:
        flood(harness, clock, upstream, downstream)
        fall(harness, clock, {upstream: LOW_CM, downstream: LOW_CM}, 9 * MINUTE)
        too_early = harness.api("/api/wadis")["wadis"][0]
        stay(harness, clock, {upstream: LOW_CM, downstream: LOW_CM}, MINUTE)
        in_time = harness.api("/api/wadis")["wadis"][0]
    finally:
        harness.stop()

    assert (too_early["allowed"], too_early["quiet_s"]) == (False, 10 * MINUTE)
    assert in_time["allowed"] is True


def test_the_watch_threshold_is_the_one_the_site_file_gives_each_wadi_node(
    tmp_path, clock, upstream, downstream
):
    raw = json.loads(DEFAULT_SITE_FILE.read_text(encoding="utf-8"))
    # The upstream node is given a threshold above the level it will report.
    next(node for node in raw["nodes"] if node["id"] == upstream.id)["watch_level_cm"] = 80.0
    path = tmp_path / "site.json"
    path.write_text(json.dumps(raw), encoding="utf-8")
    site = load_site(path)
    harness = Harness(tmp_path / "wadiwatch.db", site, clock)
    try:
        between_the_thresholds = (downstream.watch_level_cm + 80.0) / 2
        fall(
            harness,
            clock,
            {upstream: between_the_thresholds, downstream: between_the_thresholds},
            site.all_clear_quiet_s,
        )
        reasons = {
            node["id"]: node["reason"] for node in harness.api("/api/wadis")["wadis"][0]["nodes"]
        }
    finally:
        harness.stop()

    assert reasons == {upstream.id: None, downstream.id: "at_or_above_watch"}


def test_a_return_above_the_threshold_starts_the_quiet_period_again(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(
        harness, clock, site, upstream, downstream, quiet_for=site.all_clear_quiet_s - 5 * MINUTE
    )
    # A second wave: one measurement above the threshold, then below again.
    stay(harness, clock, {upstream: HIGH_CM, downstream: LOW_CM}, MINUTE)
    fall(harness, clock, {upstream: LOW_CM, downstream: LOW_CM}, site.all_clear_quiet_s - MINUTE)

    too_early = signed_in.issue_all_clear(wadi.id)
    stay(harness, clock, {upstream: LOW_CM, downstream: LOW_CM}, MINUTE)
    in_time = signed_in.issue_all_clear(wadi.id)

    assert too_early.status_code == 409
    assert refusal_of(too_early, upstream) == "below_watch_too_recently"
    assert in_time.status_code == 200


def test_the_level_behind_an_alert_and_the_level_in_a_heartbeat_count_as_levels(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(harness, clock, site, upstream, downstream)
    assert wadi_verdict(signed_in, wadi)["allowed"] is True

    # The alert refresh of the upstream node carries a level above the threshold.
    harness.ingest("alert", upstream.id, alert_level="warning", level_cm=HIGH_CM, rise_cm_min=4.0)
    by_alert = signed_in.issue_all_clear(wadi.id)
    fall(harness, clock, {upstream: LOW_CM, downstream: LOW_CM}, site.all_clear_quiet_s)
    assert wadi_verdict(signed_in, wadi)["allowed"] is True
    # So does a heartbeat of the downstream node.
    harness.ingest(
        "heartbeat",
        downstream.id,
        battery_mv=3900,
        rssi_dbm=-90,
        low_battery=False,
        tamper=False,
        level_cm=HIGH_CM,
    )
    by_heartbeat = signed_in.issue_all_clear(wadi.id)

    assert (by_alert.status_code, refusal_of(by_alert, upstream)) == (409, "at_or_above_watch")
    assert (by_heartbeat.status_code, refusal_of(by_heartbeat, downstream)) == (
        409,
        "at_or_above_watch",
    )


# An all clear for a wadi covers every wadi node on it


def test_every_wadi_node_of_the_wadi_must_have_been_below_its_threshold(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    flood(harness, clock, upstream, downstream)
    # The flood travels down: the upstream node falls, the downstream node is still high.
    harness.ingest("alert", downstream.id, alert_level="warning", level_cm=HIGH_CM, rise_cm_min=5.0)
    fall(harness, clock, {upstream: LOW_CM, downstream: HIGH_CM}, site.all_clear_quiet_s)

    refused = signed_in.issue_all_clear(wadi.id)
    fall(harness, clock, {upstream: LOW_CM, downstream: LOW_CM}, site.all_clear_quiet_s)
    accepted = signed_in.issue_all_clear(wadi.id)

    assert refused.status_code == 409
    assert refusal_of(refused, upstream) is None
    assert refusal_of(refused, downstream) == "at_or_above_watch"
    assert downstream.name in refused.json()["error"]
    assert accepted.status_code == 200
    # One command for the wadi: the gateway clears every wadi node on it.
    (command,) = harness.gateway.commands
    assert command["wadi"] == wadi.id


def test_the_site_file_says_which_wadi_each_wadi_node_is_on(site, upstream, downstream, siren):
    (wadi,) = site.wadis
    assert site.wadi_nodes_of(wadi.id) == (upstream, downstream)
    assert site.wadi_of(upstream.id) == wadi
    assert site.wadi_of(siren.id) is None
    for node in site.wadi_nodes:
        assert node.watch_level_cm > 0


# Missing or stale data: silence is not evidence


def test_an_all_clear_is_refused_when_the_gateway_has_gone_silent_during_an_alert(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(harness, clock, site, upstream, downstream)
    assert wadi_verdict(signed_in, wadi)["allowed"] is True
    # A node at Warning repeats its alert every minute. Two intervals and nothing.
    clock.advance(2 * site.alert_refresh_interval_s + 1)
    assert harness.api("/api/health")["gateway"]["stale"] is True

    response = signed_in.issue_all_clear(wadi.id)

    # The last levels heard were low, and that is not how the wadi is known to be now.
    assert response.status_code == 409
    assert response.json()["reason"] == "stale"
    assert "gateway" in response.json()["error"]
    assert harness.gateway.commands == []
    assert signed_in.alerts()[0]["in_force"] is True


def test_an_all_clear_is_refused_when_nothing_has_ever_been_received(harness, signed_in, wadi):
    response = signed_in.issue_all_clear(wadi.id)

    assert response.status_code == 409
    assert response.json()["reason"] == "stale"
    assert harness.gateway.commands == []


def test_an_all_clear_is_refused_for_a_wadi_node_in_alert_that_has_stopped_reporting(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(harness, clock, site, upstream, downstream)
    # The gateway is heard from (the downstream node goes on reporting), but the node
    # at Warning, which reports every minute, has not for more than two intervals.
    stay(harness, clock, {downstream: LOW_CM}, 2 * site.alert_refresh_interval_s + MINUTE)
    assert harness.api("/api/health")["gateway"]["stale"] is False

    response = signed_in.issue_all_clear(wadi.id)

    assert response.status_code == 409
    assert response.json()["reason"] == "wadi_nodes"
    assert refusal_of(response, upstream) == "silent"
    assert refusal_of(response, downstream) is None
    assert harness.gateway.commands == []


def test_a_wadi_node_in_dry_mode_is_not_silent_while_its_heartbeats_arrive(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(harness, clock, site, upstream, downstream)
    # The downstream node never left dry mode's pace: its level comes with its hourly
    # heartbeat only. It last reported 50 minutes ago; the upstream node reports now.
    stay(harness, clock, {upstream: LOW_CM}, 50 * MINUTE)

    response = signed_in.issue_all_clear(wadi.id)

    assert response.status_code == 200


def test_a_wadi_node_not_in_alert_is_silent_after_two_heartbeat_intervals_without_a_level(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(harness, clock, site, upstream, downstream)
    stay(harness, clock, {upstream: LOW_CM}, 2 * site.heartbeat_interval_s + MINUTE)

    response = signed_in.issue_all_clear(wadi.id)

    assert response.status_code == 409
    assert refusal_of(response, downstream) == "silent"
    assert refusal_of(response, upstream) is None


def test_an_all_clear_is_refused_for_a_wadi_node_that_has_never_reported_a_level(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    harness.ingest("alert", upstream.id, alert_level="watch")
    fall(harness, clock, {upstream: LOW_CM}, site.all_clear_quiet_s)
    # The downstream node is alive, but its heartbeat carries no level.
    harness.ingest(
        "heartbeat", downstream.id, battery_mv=3900, rssi_dbm=-90, low_battery=False, tamper=False
    )

    response = signed_in.issue_all_clear(wadi.id)

    assert response.status_code == 409
    assert refusal_of(response, downstream) == "no_level"
    assert refusal_of(response, upstream) is None


def test_levels_forwarded_late_after_an_uplink_outage_count_for_when_they_were_measured(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    flood(harness, clock, upstream, downstream)
    flooded_at = int(clock())
    # The uplink is down for the whole of the quiet period and a little more. When it
    # returns the gateway forwards what it held, each message with its own time.
    outage = site.all_clear_quiet_s + 5 * MINUTE
    clock.advance(outage)
    for elapsed in range(MINUTE, outage + 1, MINUTE):
        for node in (upstream, downstream):
            held = harness.message("reading", node.id, level_cm=LOW_CM, rise_cm_min=0.0)
            held["ts"] = flooded_at + elapsed
            assert harness.ingest_raw(json.dumps(held)).status == "stored"

    response = signed_in.issue_all_clear(wadi.id)

    # The backend received them all a moment ago, and they span the quiet period.
    assert response.status_code == 200


def test_everyone_can_see_whether_an_all_clear_would_be_accepted_and_why_not(
    harness, visitor, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(
        harness, clock, site, upstream, downstream, quiet_for=site.all_clear_quiet_s - 10 * MINUTE
    )

    refused = wadi_verdict(visitor, wadi)
    stay(harness, clock, {upstream: LOW_CM, downstream: LOW_CM}, 10 * MINUTE)
    allowed = wadi_verdict(visitor, wadi)

    assert refused["wadi"] == {"id": wadi.id, "name": wadi.name}
    assert refused["allowed"] is False
    assert refused["reason"] == "wadi_nodes"
    assert upstream.name in refused["refusal"]
    assert [node["id"] for node in refused["nodes"]] == [upstream.id, downstream.id]
    judged = refused["nodes"][0]
    assert judged["watch_level_cm"] == upstream.watch_level_cm
    assert judged["level_cm"] == LOW_CM
    assert judged["allowed_from"] == int(clock())
    assert (allowed["allowed"], allowed["reason"], allowed["refusal"]) == (True, None, None)


# A level a wadi node could not measure again is not evidence


def cannot_measure(harness, clock, blind, others, seconds):
    """For `seconds` the level sensor of the wadi node `blind`, which is at Warning,
    gives no usable reading: its alert refresh repeats the last level it measured,
    and says so. The wadi nodes in `others` go on reporting the levels given. One
    report each a minute, the last of them now."""
    for _ in range(seconds // MINUTE):
        clock.advance(MINUTE)
        harness.ingest(
            "alert",
            blind.id,
            alert_level="warning",
            level_cm=LOW_CM,
            rise_cm_min=0.0,
            refresh=True,
            level_repeated=True,
        )
        for node, level_cm in others.items():
            harness.ingest("reading", node.id, level_cm=level_cm, rise_cm_min=0.0)


def test_an_all_clear_is_refused_when_a_wadi_node_only_repeats_a_level_it_measured_earlier(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    flood(harness, clock, upstream, downstream)
    # The upstream node measures one level below its Watch threshold. Then its sensor
    # fails, and for longer than the quiet period every alert refresh repeats that level.
    fall(harness, clock, {upstream: LOW_CM, downstream: LOW_CM}, quiet_for=0)
    measured_at = int(clock())
    cannot_measure(harness, clock, upstream, {downstream: LOW_CM}, site.all_clear_quiet_s + MINUTE)

    response = signed_in.issue_all_clear(wadi.id)

    assert response.status_code == 409
    assert response.json()["reason"] == "wadi_nodes"
    assert refusal_of(response, upstream) == "not_measuring"
    assert refusal_of(response, downstream) is None
    judged = next(node for node in response.json()["nodes"] if node["id"] == upstream.id)
    assert judged["measured_ts"] == measured_at
    assert judged["below_since"] is None
    # The officer reads which wadi node it is and for how long it has not measured.
    assert upstream.name in response.json()["error"]
    assert "31 min" in response.json()["error"]
    assert harness.gateway.commands == []


def test_the_time_a_wadi_node_could_not_measure_does_not_count_towards_the_quiet_period(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    flood(harness, clock, upstream, downstream)
    fall(harness, clock, {upstream: LOW_CM, downstream: LOW_CM}, quiet_for=10 * MINUTE)
    # Nobody knows where the water stood at the upstream node in these five minutes.
    cannot_measure(harness, clock, upstream, {downstream: LOW_CM}, 5 * MINUTE)
    # Then the node measures again, and the level is below the threshold.
    stay(harness, clock, {upstream: LOW_CM, downstream: LOW_CM}, MINUTE)
    measuring_again_at = int(clock())
    stay(harness, clock, {upstream: LOW_CM, downstream: LOW_CM}, site.all_clear_quiet_s - MINUTE)

    too_early = signed_in.issue_all_clear(wadi.id)
    stay(harness, clock, {upstream: LOW_CM, downstream: LOW_CM}, MINUTE)
    in_time = signed_in.issue_all_clear(wadi.id)

    # The quiet period counts from the first level measured after the gap.
    assert too_early.status_code == 409
    assert refusal_of(too_early, upstream) == "below_watch_too_recently"
    judged = next(node for node in too_early.json()["nodes"] if node["id"] == upstream.id)
    assert judged["below_since"] == measuring_again_at
    assert in_time.status_code == 200


def test_an_all_clear_is_refused_when_a_heartbeat_repeats_a_level_the_wadi_node_can_no_longer_measure(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(harness, clock, site, upstream, downstream)
    assert wadi_verdict(signed_in, wadi)["allowed"] is True
    # The downstream node is in dry mode and reports its level in its heartbeat only.
    # Its sensor has stopped giving a reading since.
    harness.ingest(
        "heartbeat",
        downstream.id,
        battery_mv=3900,
        rssi_dbm=-90,
        low_battery=False,
        tamper=False,
        level_cm=LOW_CM,
        level_repeated=True,
    )

    response = signed_in.issue_all_clear(wadi.id)

    assert response.status_code == 409
    assert refusal_of(response, downstream) == "not_measuring"
    assert refusal_of(response, upstream) is None
    assert downstream.name in response.json()["error"]


def test_a_wadi_node_that_reports_a_level_without_ever_having_measured_one_blocks_the_all_clear(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    harness.ingest("alert", upstream.id, alert_level="watch")
    fall(harness, clock, {upstream: LOW_CM}, site.all_clear_quiet_s)
    # Rain alone raised Watch at the downstream node, whose sensor has never given a level.
    harness.ingest(
        "alert", downstream.id, alert_level="watch", level_cm=0.0, rise_cm_min=0.0,
        refresh=False, level_repeated=True,
    )

    response = signed_in.issue_all_clear(wadi.id)

    assert response.status_code == 409
    assert refusal_of(response, downstream) == "not_measuring"
    judged = next(node for node in response.json()["nodes"] if node["id"] == downstream.id)
    assert judged["measured_ts"] is None


# Only a signed-in officer


def test_an_all_clear_requires_a_signed_in_officer(
    harness, visitor, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(harness, clock, site, upstream, downstream)

    response = visitor.issue_all_clear(wadi.id)

    assert response.status_code == 401
    assert harness.gateway.commands == []
    assert visitor.alerts()[0]["in_force"] is True
    assert visitor.alerts()[0]["all_clear_sent"] is None


def test_a_made_up_session_cookie_issues_no_all_clear(
    harness, visitor, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(harness, clock, site, upstream, downstream)
    visitor.client.cookies.set("wadiwatch_session", "not-a-session")

    response = visitor.issue_all_clear(wadi.id, headers={CSRF_HEADER: "not-a-token"})

    assert response.status_code == 401
    assert harness.gateway.commands == []


def test_a_session_that_has_ended_issues_no_all_clear(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(harness, clock, site, upstream, downstream)
    assert signed_in.sign_out().status_code == 200

    response = signed_in.issue_all_clear(wadi.id)

    assert response.status_code == 401
    assert harness.gateway.commands == []


def test_an_all_clear_without_the_csrf_token_is_refused(
    harness, signed_in, second_officer, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(harness, clock, site, upstream, downstream)
    other = Browser(harness)
    other.sign_in(second_officer)

    without = signed_in.issue_all_clear(wadi.id, headers={})
    wrong = signed_in.issue_all_clear(wadi.id, headers={CSRF_HEADER: "x" * 43})
    of_another_session = signed_in.issue_all_clear(wadi.id, headers={CSRF_HEADER: other.csrf_token})

    assert (without.status_code, wrong.status_code, of_another_session.status_code) == (403, 403, 403)
    assert harness.gateway.commands == []


def test_an_all_clear_sent_from_another_site_is_refused_and_one_from_the_dashboard_is_not(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(harness, clock, site, upstream, downstream)
    own_origin = str(signed_in.client.base_url).rstrip("/")

    from_elsewhere = signed_in.issue_all_clear(
        wadi.id, headers={CSRF_HEADER: signed_in.csrf_token, "Origin": "https://elsewhere.example"}
    )
    assert from_elsewhere.status_code == 403
    assert harness.gateway.commands == []

    from_the_dashboard = signed_in.issue_all_clear(
        wadi.id, headers={CSRF_HEADER: signed_in.csrf_token, "Origin": own_origin}
    )
    assert from_the_dashboard.status_code == 200


def test_the_sign_in_check_comes_before_the_rule(harness, visitor, clock, wadi, upstream, downstream):
    flood(harness, clock, upstream, downstream)

    # A visitor is not told why the all clear would be refused by asking for one.
    assert visitor.issue_all_clear(wadi.id).status_code == 401
    assert visitor.issue_all_clear(10**6).status_code == 401


def test_there_is_no_all_clear_for_a_wadi_the_site_does_not_have(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(harness, clock, site, upstream, downstream)

    assert signed_in.issue_all_clear(wadi.id + 1).status_code == 404
    assert signed_in.issue_all_clear(0).status_code == 404
    assert signed_in.client.post("/api/wadis/all/all-clear").status_code in (404, 422)
    assert harness.gateway.commands == []


# The way to the gateway


def test_no_all_clear_is_issued_when_the_command_cannot_be_sent(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(harness, clock, site, upstream, downstream)
    harness.gateway.reachable = False

    response = signed_in.issue_all_clear(wadi.id)

    assert response.status_code == 503
    assert response.json()["error"]
    assert signed_in.alerts()[0]["in_force"] is True
    assert signed_in.alerts()[0]["all_clear_sent"] is None

    # The officer tries again when the connection is back: a new command, a new number.
    harness.gateway.reachable = True
    again = signed_in.issue_all_clear(wadi.id)
    assert again.status_code == 200
    (command,) = harness.gateway.commands
    assert command["command"] == again.json()["command"]


def test_a_backend_with_no_connection_to_a_gateway_issues_no_all_clear(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    # As the backend runs when it is fed over HTTP only.
    harness.backend.downlink = None
    flood_that_has_passed(harness, clock, site, upstream, downstream)

    response = signed_in.issue_all_clear(wadi.id)

    assert response.status_code == 503
    assert signed_in.alerts()[0]["in_force"] is True
    with pytest.raises(GatewayUnreachable):
        harness.backend.issue_all_clear(wadi.id, "officer1")


def test_the_command_carries_the_time_it_was_issued_and_a_number_of_its_own(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(harness, clock, site, upstream, downstream)
    first = signed_in.issue_all_clear(wadi.id).json()
    harness.gateway_relays(harness.gateway.commands[0])
    # A second event, and its all clear.
    clock.advance(3600)
    flood_that_has_passed(harness, clock, site, upstream, downstream)

    second = signed_in.issue_all_clear(wadi.id).json()

    sent_first, sent_second = harness.gateway.commands
    assert sent_first == {
        "v": 1,
        "kind": "all_clear",
        "command": first["command"],
        "ts": first["ts"],
        "wadi": wadi.id,
    }
    assert sent_second["ts"] == int(clock())
    assert sent_second["command"] > sent_first["command"] >= 1
    assert second["command"] == sent_second["command"]


def test_a_second_request_while_the_first_is_on_its_way_sends_nothing_more(
    harness, signed_in, officer, second_officer, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(harness, clock, site, upstream, downstream)
    first = signed_in.issue_all_clear(wadi.id)
    colleague = Browser(harness)
    colleague.sign_in(second_officer)
    clock.advance(20)

    second = colleague.issue_all_clear(wadi.id)

    # The first stands, with the officer who issued it.
    assert second.status_code == 200
    assert second.json() == first.json()
    assert second.json()["officer"] == officer.name
    assert len(harness.gateway.commands) == 1


def test_a_command_the_gateway_never_reported_can_be_issued_again_once_it_is_no_longer_fresh(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(harness, clock, site, upstream, downstream)
    first = signed_in.issue_all_clear(wadi.id)
    # The gateway does not report an all clear. The nodes go on reporting.
    stay(harness, clock, {upstream: LOW_CM, downstream: LOW_CM}, 2 * MINUTE)
    assert 2 * MINUTE >= site.all_clear_command_valid_s
    assert signed_in.alerts()[0]["all_clear_sent"] is None

    again = signed_in.issue_all_clear(wadi.id)

    assert again.status_code == 200
    assert again.json()["command"] != first.json()["command"]
    assert len(harness.gateway.commands) == 2


# In force only once the gateway has reported it; then in the alert log with officer and time


def test_the_alert_stays_in_force_until_the_gateway_reports_the_all_clear(
    harness, signed_in, visitor, officer, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(harness, clock, site, upstream, downstream)

    signed_in.issue_all_clear(wadi.id)
    issued_at = int(clock())
    clock.advance(30)

    # Sent, not yet done: the sirens are still sounding for all the backend knows.
    (alert,) = signed_in.alerts()
    assert alert["in_force"] is True
    assert alert["wadi"] == {"id": wadi.id, "name": wadi.name}
    assert alert["all_clear_sent"] == {"ts": issued_at, "officer": officer.name}
    assert visitor.alerts()[0]["all_clear_sent"] == {"ts": issued_at}
    assert signed_in.all_clears() == []
    assert harness.nodes()[upstream.id]["alert_level"] == "warning"

    harness.gateway_relays(harness.gateway.commands[0])

    (alert,) = signed_in.alerts()
    assert alert["in_force"] is False
    assert alert["all_clear_sent"] is None
    assert harness.nodes()[upstream.id]["alert_level"] == "none"


def test_the_alert_log_shows_the_all_clear_with_the_officer_and_the_time(
    harness, signed_in, visitor, officer, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(harness, clock, site, upstream, downstream)
    command = signed_in.issue_all_clear(wadi.id).json()
    issued_at = int(clock())
    clock.advance(45)
    harness.gateway_relays(harness.gateway.commands[0])
    relayed_at = int(clock())

    entries = signed_in.all_clears()

    # One entry for each wadi node of the wadi, from the gateway, at the time it
    # transmitted; with who issued the all clear and when.
    assert {entry["wadi_node"] for entry in entries} == {upstream.id, downstream.id}
    for entry in entries:
        assert entry["node"] == site.gateway.id
        assert entry["ts"] == relayed_at
        assert entry["command"] == command["command"]
        assert entry["issued"] == {"ts": issued_at, "officer": officer.name}

    # The public log shows that and when, not who.
    public = visitor.client.get("/api/alert-log")
    assert officer.name not in public.text
    for entry in public.json()["entries"]:
        if entry["type"] == "all_clear":
            assert entry["issued"] == {"ts": issued_at}


def test_the_officer_and_time_of_an_all_clear_survive_a_restart(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(harness, clock, site, upstream, downstream)
    signed_in.issue_all_clear(wadi.id)
    harness.gateway_relays(harness.gateway.commands[0])
    before = signed_in.alert_log()

    harness.restart()

    assert before[0]["issued"] is not None
    assert signed_in.alert_log() == before


def test_an_all_clear_the_backend_did_not_issue_is_logged_without_an_officer(
    harness, signed_in, site, upstream
):
    harness.ingest("alert", upstream.id, alert_level="warning")
    # Written by hand, or relayed for a command this database knows nothing of.
    harness.ingest("all_clear", site.gateway.id, wadi_node=upstream.id)
    harness.ingest("alert", upstream.id, alert_level="warning")
    harness.ingest("all_clear", site.gateway.id, wadi_node=upstream.id, command=999)

    with_unknown_command, without_command = signed_in.all_clears()

    assert (without_command["command"], without_command["issued"]) == (None, None)
    assert (with_unknown_command["command"], with_unknown_command["issued"]) == (999, None)


def test_an_all_clear_the_gateway_transmits_again_for_a_node_that_missed_it_is_one_entry(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(harness, clock, site, upstream, downstream)
    command = signed_in.issue_all_clear(wadi.id).json()["command"]
    harness.gateway_relays(harness.gateway.commands[0])
    clock.advance(MINUTE)

    # The downstream node's heartbeat shows the gateway that it has not heard.
    harness.ingest("all_clear", site.gateway.id, wadi_node=downstream.id, command=command)

    assert [entry["wadi_node"] for entry in signed_in.all_clears()] == [downstream.id, upstream.id]


def test_an_alert_refresh_between_two_all_clears_of_one_command_shows_as_what_happened(
    harness, signed_in, clock, site, wadi, upstream, downstream
):
    flood_that_has_passed(harness, clock, site, upstream, downstream)
    command = signed_in.issue_all_clear(wadi.id).json()["command"]
    harness.gateway_relays(harness.gateway.commands[0])
    clock.advance(MINUTE)

    # The upstream node missed the all clear and sent its alert again, which sounded
    # the siren again until the gateway answered.
    harness.ingest("alert", upstream.id, alert_level="warning", refresh=True)
    assert signed_in.alerts()[0]["in_force"] is True
    harness.ingest("all_clear", site.gateway.id, wadi_node=upstream.id, command=command)

    log = signed_in.alert_log()
    assert [(entry["type"], entry.get("wadi_node", entry["node"])) for entry in log[:3]] == [
        ("all_clear", upstream.id),
        ("alert", upstream.id),
        ("all_clear", downstream.id),
    ]
    assert log[1]["in_force"] is False
    assert harness.nodes()[upstream.id]["alert_level"] == "none"


def test_a_siren_node_acknowledgement_that_arrives_behind_the_all_clear_leaves_no_alert_in_force(
    harness, signed_in, clock, site, wadi, upstream, downstream, siren
):
    flood_that_has_passed(harness, clock, site, upstream, downstream, alert_level="evacuate")
    assert signed_in.issue_all_clear(wadi.id).status_code == 200
    # The node's last alert refresh, the gateway's answer to it, and only then the
    # siren node's acknowledgement of that refresh.
    refresh = harness.message("alert", upstream.id, alert_level="evacuate", refresh=True)
    harness.ingest_raw(json.dumps(refresh))
    harness.gateway_relays(harness.gateway.commands[0])
    harness.ingest(
        "acknowledgement",
        siren.id,
        wadi_node=upstream.id,
        alert_level="evacuate",
        alert_counter=refresh["counter"],
    )

    nodes = harness.nodes()

    assert nodes[upstream.id]["alert_level"] == "none"
    assert nodes[siren.id]["alert_level"] == "none"


def test_a_siren_node_acknowledgement_of_an_alert_after_the_all_clear_is_in_force(
    harness, signed_in, clock, site, wadi, upstream, downstream, siren
):
    flood_that_has_passed(harness, clock, site, upstream, downstream)
    signed_in.issue_all_clear(wadi.id)
    harness.gateway_relays(harness.gateway.commands[0])

    # A new event.
    raised = harness.message("alert", upstream.id, alert_level="warning", refresh=False)
    harness.ingest_raw(json.dumps(raised))
    harness.ingest(
        "acknowledgement",
        siren.id,
        wadi_node=upstream.id,
        alert_level="warning",
        alert_counter=raised["counter"],
    )

    assert harness.nodes()[siren.id]["alert_level"] == "warning"


# The stand-in gateway for trying the dashboard without hardware


def test_with_the_simulated_gateway_an_all_clear_comes_back_as_the_gateway_would_report_it(
    harness, signed_in, officer, clock, site, wadi, upstream, downstream
):
    harness.backend.downlink = SimulatedGateway(harness.backend, clock)
    # The stand-in carries on from the gateway's own counter, as a replayed scenario leaves it.
    harness.ingest("all_clear", site.gateway.id, wadi_node=upstream.id)
    flood_that_has_passed(harness, clock, site, upstream, downstream)

    response = signed_in.issue_all_clear(wadi.id)

    assert response.status_code == 200
    all_clears = signed_in.all_clears()[:2]
    assert {entry["wadi_node"] for entry in all_clears} == {upstream.id, downstream.id}
    for entry in all_clears:
        assert entry["node"] == site.gateway.id
        assert entry["command"] == response.json()["command"]
        assert entry["issued"] == {"ts": int(clock()), "officer": officer.name}
    assert signed_in.alerts()[0]["in_force"] is False
    assert harness.nodes()[upstream.id]["alert_level"] == "none"
    # Nothing is left on its way: the answer came back at once.
    assert signed_in.alerts()[0]["all_clear_sent"] is None
