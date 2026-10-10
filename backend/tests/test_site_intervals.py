"""The intervals that node health and staleness are judged against come from the site file."""

import json

import pytest

from conftest import Harness
from wadiwatch_backend.site import DEFAULT_SITE_FILE, INTERVALS, SiteError, load_site


def site_file_with(tmp_path, change):
    raw = json.loads(DEFAULT_SITE_FILE.read_text(encoding="utf-8"))
    change(raw)
    path = tmp_path / "site.json"
    path.write_text(json.dumps(raw), encoding="utf-8")
    return path


def test_prototype_site_states_every_interval(site):
    for name in INTERVALS:
        assert getattr(site, name) > 0, name
    # A heartbeat is rarer than a dry-mode measurement, which is rarer than an alert refresh.
    assert site.heartbeat_interval_s > site.dry_mode_interval_s > site.alert_refresh_interval_s


@pytest.mark.parametrize("name", INTERVALS)
def test_site_file_without_an_interval_is_refused(tmp_path, name):
    path = site_file_with(tmp_path, lambda raw: raw.pop(name))

    with pytest.raises(SiteError, match=name):
        load_site(path)


@pytest.mark.parametrize("value", [0, -60, 1.5, "3600", True])
def test_interval_must_be_a_positive_whole_number_of_seconds(tmp_path, value):
    path = site_file_with(tmp_path, lambda raw: raw.update(heartbeat_interval_s=value))

    with pytest.raises(SiteError, match="heartbeat_interval_s"):
        load_site(path)


def test_faulty_rule_follows_the_interval_in_the_site_file(tmp_path, clock, upstream):
    path = site_file_with(tmp_path, lambda raw: raw.update(heartbeat_interval_s=600))
    harness = Harness(tmp_path / "wadiwatch.db", load_site(path), clock)
    try:
        harness.ingest(
            "heartbeat", upstream.id, battery_mv=3900, rssi_dbm=-90, low_battery=False, tamper=False
        )
        clock.advance(2 * 600 + 1)

        health = harness.api("/api/health")

        assert health["faulty_after_s"] == 2 * 600
        assert next(node for node in health["nodes"] if node["id"] == upstream.id)["faulty"] is True
    finally:
        harness.stop()
