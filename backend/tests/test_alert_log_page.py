"""The dashboard page carries the alert log panel, the sign-in form and the Evacuate request."""

import re

from officer_support import ALERT_LOG_URL, CSRF_HEADER, SESSION_URL


def alert_log_script(harness):
    page = harness.client.get("/").text
    url = re.search(r'<script[^>]+src="([^"]*alert_log\.js)"', page).group(1)
    response = harness.client.get("/" + url.lstrip("/"))
    assert response.status_code == 200
    return response.text


def test_the_page_has_a_place_for_the_alert_log_and_the_evacuate_request(harness):
    page = harness.client.get("/").text

    assert 'id="alert-log"' in page
    assert 'id="acknowledgement-requests"' in page
    assert re.search(r'<link[^>]+href="[^"]*alert_log\.css"', page)


def test_the_sign_in_form_asks_for_a_name_and_a_password_the_browser_hides(harness):
    page = harness.client.get("/").text

    form = re.search(r'<form[^>]*id="sign-in-form".*?</form>', page, re.DOTALL).group(0)
    assert re.search(r'<input[^>]+type="password"[^>]*autocomplete="current-password"', form)
    assert re.search(r'<input[^>]+autocomplete="username"', form)
    # The script sends the form as JSON. Without the script the form only closes its
    # dialog: it can never put the password into the page address.
    assert 'method="dialog"' in form
    assert "value=" not in form


def test_the_script_reads_the_alert_log_and_signs_in_through_the_api(harness):
    script = alert_log_script(harness)

    assert ALERT_LOG_URL.lstrip("/") in script
    assert SESSION_URL.lstrip("/") in script
    assert CSRF_HEADER in script
    assert "acknowledgement_requested" in script


def test_the_script_does_not_keep_sign_in_secrets_in_browser_storage(harness):
    script = alert_log_script(harness)

    assert "localStorage" not in script
    assert "sessionStorage" not in script
    assert "document.cookie" not in script


# The all clear


def test_the_page_asks_before_an_all_clear_is_issued(harness):
    page = harness.client.get("/").text

    form = re.search(r'<form[^>]*id="all-clear-form".*?</form>', page, re.DOTALL).group(0)
    # Cancel comes first and is not the button that submits.
    assert form.index('id="all-clear-cancel"') < form.index('id="all-clear-submit"')
    assert re.search(r'<button[^>]+id="all-clear-cancel"[^>]+type="button"', form)
    assert re.search(r'<button[^>]+id="all-clear-submit"[^>]+type="submit"', form)
    assert "closing SMS" in form


def test_the_script_issues_the_all_clear_through_the_api_with_the_csrf_token(harness):
    script = alert_log_script(harness)

    assert "api/wadis" in script
    assert "/all-clear" in script
    issue = script[script.index("async function issueAllClear") :]
    issue = issue[: issue.index("function allClearControl")]
    assert 'method: "POST"' in issue
    assert "CSRF_HEADER" in issue
    # What the backend refuses, and why, is shown to the officer as the backend words it.
    assert "409" in issue
    assert ".error" in issue


def test_the_script_shows_who_issued_an_all_clear_and_one_that_is_on_its_way(harness):
    script = alert_log_script(harness)

    assert "entry.issued" in script
    assert "all_clear_sent" in script
    # The control is the backend's to allow: the script never works the rule out itself.
    assert "verdict.allowed" in script
    assert "watch_level_cm" not in script
