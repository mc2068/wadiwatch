"""The dashboard page carries the curves, the node health panel and the stale-data warning."""

import re


def health_script(harness):
    page = harness.client.get("/").text
    script_url = re.search(r'<script[^>]+src="([^"]*health\.js)"', page).group(1)
    return harness.client.get("/" + script_url.lstrip("/")).text


def test_dashboard_script_reads_the_health_and_readings_apis(harness):
    script = health_script(harness)

    assert "api/health" in script
    assert "/readings" in script


def test_dashboard_page_has_a_place_for_curves_health_and_the_stale_data_warning(harness):
    page = harness.client.get("/").text

    for element_id in ("gateway-status", "curves", "health-rows"):
        assert f'id="{element_id}"' in page, element_id


def test_curves_are_drawn_without_a_charting_library(harness):
    page = harness.client.get("/").text
    outside_scripts = re.findall(r'<script[^>]+src="(https?://[^"]+)"', page)

    # Leaflet, for the map, stays the only script from outside.
    assert all("leaflet" in url for url in outside_scripts), outside_scripts
