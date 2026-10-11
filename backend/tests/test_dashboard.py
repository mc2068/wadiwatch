"""The backend serves the dashboard page and the files it loads."""

import re


def test_dashboard_page_is_served_at_the_root(harness):
    response = harness.client.get("/")

    assert response.status_code == 200
    assert response.headers["content-type"].startswith("text/html")
    assert "WadiWatch" in response.text
    assert "leaflet" in response.text.lower()


def test_every_local_file_the_page_loads_is_served(harness):
    page = harness.client.get("/").text
    local_files = [
        url
        for url in re.findall(r'(?:src|href)="([^"]+)"', page)
        if not url.startswith(("http://", "https://"))
    ]

    assert local_files, "the page should load its own script and stylesheet"
    for url in local_files:
        assert harness.client.get("/" + url.lstrip("/")).status_code == 200, url


def test_dashboard_script_reads_the_nodes_api(harness):
    page = harness.client.get("/").text
    script_url = re.search(r'<script[^>]+src="([^"]*dashboard\.js)"', page).group(1)

    script = harness.client.get("/" + script_url.lstrip("/")).text

    assert "api/nodes" in script
