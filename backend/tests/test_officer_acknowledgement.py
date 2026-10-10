"""An officer acknowledges an alert: who may, what is recorded, and what the dashboard is asked to show."""

from officer_support import CSRF_HEADER, Browser


def raise_alert(harness, browser, node, alert_level):
    """Ingest an alert from `node` and return its entry in the alert log."""
    harness.ingest("alert", node.id, alert_level=alert_level)
    return browser.alerts()[0]


def test_an_alert_starts_without_an_officer_acknowledgement(harness, visitor, upstream):
    alert = raise_alert(harness, visitor, upstream, "warning")

    assert alert["acknowledgement"] is None


def test_an_unauthenticated_acknowledgement_request_is_refused(harness, visitor, upstream):
    alert = raise_alert(harness, visitor, upstream, "evacuate")

    response = visitor.acknowledge(alert["id"])

    assert response.status_code == 401
    assert visitor.alerts()[0]["acknowledgement"] is None
    assert visitor.alerts()[0]["acknowledgement_requested"] is True


def test_a_made_up_session_cookie_is_refused(harness, visitor, upstream):
    alert = raise_alert(harness, visitor, upstream, "warning")
    visitor.client.cookies.set("wadiwatch_session", "not-a-session")

    response = visitor.acknowledge(alert["id"], headers={CSRF_HEADER: "not-a-token"})

    assert response.status_code == 401
    assert visitor.alerts()[0]["acknowledgement"] is None


def test_a_signed_in_officer_acknowledges_an_alert_and_who_and_when_are_recorded(
    harness, signed_in, officer, clock, upstream
):
    alert = raise_alert(harness, signed_in, upstream, "warning")
    clock.advance(95)

    response = signed_in.acknowledge(alert["id"])

    assert response.status_code == 200
    recorded = {"ts": int(clock()), "officer": officer.name}
    assert response.json()["acknowledgement"] == recorded
    clock.advance(600)
    assert signed_in.alerts()[0]["acknowledgement"] == recorded


def test_the_public_log_shows_that_and_when_but_not_who(
    harness, signed_in, visitor, officer, clock, upstream
):
    alert = raise_alert(harness, signed_in, upstream, "warning")
    clock.advance(30)
    signed_in.acknowledge(alert["id"])

    public = visitor.client.get("/api/alert-log")

    assert public.json()["entries"][0]["acknowledgement"] == {"ts": int(clock())}
    assert officer.name not in public.text
    assert public.json()["signed_in_as"] is None
    assert signed_in.client.get("/api/alert-log").json()["signed_in_as"] == officer.name


def test_only_the_acknowledged_alert_is_marked(harness, signed_in, upstream, downstream):
    raise_alert(harness, signed_in, upstream, "warning")
    downstream_alert = raise_alert(harness, signed_in, downstream, "warning")

    signed_in.acknowledge(downstream_alert["id"])

    acknowledged = {alert["node"]: alert["acknowledgement"] for alert in signed_in.alerts()}
    assert acknowledged[downstream.id] is not None
    assert acknowledged[upstream.id] is None


def test_the_first_acknowledgement_stands_when_a_second_officer_acknowledges(
    harness, signed_in, officer, second_officer, clock, upstream
):
    alert = raise_alert(harness, signed_in, upstream, "evacuate")
    signed_in.acknowledge(alert["id"])
    first = {"ts": int(clock()), "officer": officer.name}
    colleague = Browser(harness)
    colleague.sign_in(second_officer)
    clock.advance(240)

    response = colleague.acknowledge(alert["id"])

    assert response.status_code == 200
    assert response.json()["acknowledgement"] == first
    assert colleague.alerts()[0]["acknowledgement"] == first


def test_a_refresh_of_the_alert_does_not_undo_the_acknowledgement(
    harness, signed_in, clock, upstream
):
    alert = raise_alert(harness, signed_in, upstream, "evacuate")
    signed_in.acknowledge(alert["id"])
    clock.advance(60)

    harness.ingest("alert", upstream.id, alert_level="evacuate")

    (still,) = signed_in.alerts()
    assert still["id"] == alert["id"]
    assert still["acknowledgement"] is not None


def test_only_alerts_can_be_acknowledged(harness, signed_in, site, upstream, siren):
    harness.ingest("test", siren.id)
    harness.ingest("all_clear", site.gateway.id, wadi_node=upstream.id)
    all_clear, test = signed_in.alert_log()
    harness.ingest("reading", upstream.id, level_cm=10.0, rise_cm_min=0.0)

    assert signed_in.acknowledge(test["id"]).status_code == 404
    assert signed_in.acknowledge(all_clear["id"]).status_code == 404
    assert signed_in.acknowledge(all_clear["id"] + 1).status_code == 404
    assert signed_in.acknowledge(10**9).status_code == 404
    assert signed_in.client.post("/api/alerts/latest/acknowledgement").status_code in (404, 422)


def test_the_acknowledgement_survives_a_restart(harness, signed_in, visitor, upstream):
    alert = raise_alert(harness, signed_in, upstream, "evacuate")
    signed_in.acknowledge(alert["id"])
    before = signed_in.alerts()

    harness.restart()

    assert before[0]["acknowledgement"] is not None
    assert signed_in.alerts() == before
    assert visitor.alerts()[0]["acknowledgement"] == {"ts": before[0]["acknowledgement"]["ts"]}


# Protection against requests another site makes the officer's browser send


def test_an_acknowledgement_without_the_csrf_token_is_refused(harness, signed_in, upstream):
    alert = raise_alert(harness, signed_in, upstream, "evacuate")

    without = signed_in.acknowledge(alert["id"], headers={})
    wrong = signed_in.acknowledge(alert["id"], headers={CSRF_HEADER: "x" * 43})

    assert (without.status_code, wrong.status_code) == (403, 403)
    assert signed_in.alerts()[0]["acknowledgement"] is None


def test_the_csrf_token_of_another_session_is_refused(
    harness, signed_in, second_officer, upstream
):
    alert = raise_alert(harness, signed_in, upstream, "evacuate")
    other = Browser(harness)
    other.sign_in(second_officer)

    response = signed_in.acknowledge(alert["id"], headers={CSRF_HEADER: other.csrf_token})

    assert response.status_code == 403
    assert signed_in.alerts()[0]["acknowledgement"] is None


def test_an_acknowledgement_sent_from_another_site_is_refused(harness, signed_in, upstream):
    alert = raise_alert(harness, signed_in, upstream, "evacuate")

    response = signed_in.acknowledge(
        alert["id"],
        headers={CSRF_HEADER: signed_in.csrf_token, "Origin": "https://elsewhere.example"},
    )

    assert response.status_code == 403
    assert signed_in.alerts()[0]["acknowledgement"] is None


def test_an_acknowledgement_from_the_dashboard_own_origin_is_accepted(
    harness, signed_in, upstream
):
    alert = raise_alert(harness, signed_in, upstream, "evacuate")
    own_origin = str(signed_in.client.base_url).rstrip("/")

    response = signed_in.acknowledge(
        alert["id"], headers={CSRF_HEADER: signed_in.csrf_token, "Origin": own_origin}
    )

    assert response.status_code == 200


# The request the dashboard keeps showing at Evacuate


def test_an_evacuate_alert_asks_for_acknowledgement_until_an_officer_gives_it(
    harness, signed_in, visitor, clock, upstream
):
    alert = raise_alert(harness, visitor, upstream, "evacuate")
    assert alert["acknowledgement_requested"] is True

    # It keeps asking however long nobody answers, through every refresh.
    for _ in range(3):
        clock.advance(3600)
        harness.ingest("alert", upstream.id, alert_level="evacuate")
    assert visitor.alerts()[0]["acknowledgement_requested"] is True

    signed_in.acknowledge(alert["id"])

    assert visitor.alerts()[0]["acknowledgement_requested"] is False


def test_only_evacuate_asks_for_acknowledgement(harness, visitor, upstream, downstream):
    harness.ingest("alert", upstream.id, alert_level="watch")
    harness.ingest("alert", upstream.id, alert_level="warning")
    harness.ingest("alert", downstream.id, alert_level="evacuate")

    requested = {
        (alert["node"], alert["alert_level"]): alert["acknowledgement_requested"]
        for alert in visitor.alerts()
    }

    assert requested == {
        (upstream.id, "watch"): False,
        (upstream.id, "warning"): False,
        (downstream.id, "evacuate"): True,
    }


def test_acknowledging_a_warning_does_not_answer_the_evacuate_that_follows(
    harness, signed_in, upstream
):
    warning = raise_alert(harness, signed_in, upstream, "warning")
    signed_in.acknowledge(warning["id"])

    evacuate = raise_alert(harness, signed_in, upstream, "evacuate")

    assert evacuate["acknowledgement"] is None
    assert evacuate["acknowledgement_requested"] is True


def test_the_request_ends_with_the_all_clear(harness, visitor, site, upstream):
    raise_alert(harness, visitor, upstream, "evacuate")

    harness.ingest("all_clear", site.gateway.id, wadi_node=upstream.id)

    (alert,) = visitor.alerts()
    assert alert["acknowledgement"] is None
    assert alert["acknowledgement_requested"] is False
