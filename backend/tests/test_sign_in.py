"""Officers sign in with a test account; the session lives on the server."""

from http.cookies import SimpleCookie

import pytest
from fastapi.testclient import TestClient
from officer_support import SESSION_URL, Account, Browser

from wadiwatch_backend.app import create_app
from wadiwatch_backend.officers import SESSION_SECONDS, hash_password, verify_password


def session_cookie(response):
    """The session cookie a response sets, with its attributes."""
    cookies = SimpleCookie()
    cookies.load(response.headers["set-cookie"])
    (morsel,) = cookies.values()
    return morsel


def test_nobody_is_signed_in_to_begin_with(visitor):
    assert visitor.session() == {"signed_in": False}


def test_an_officer_signs_in_with_name_and_password(harness, visitor, officer):
    response = visitor.sign_in(officer)

    assert response.status_code == 200
    assert response.json()["officer"] == officer.name
    session = visitor.session()
    assert session["signed_in"] is True
    assert session["officer"] == officer.name
    # The page asks again after a reload and gets the same token for its requests.
    assert session["csrf_token"] == response.json()["csrf_token"]


def test_the_session_cookie_is_kept_from_scripts_and_from_other_sites(visitor, officer):
    response = visitor.sign_in(officer)

    cookie = session_cookie(response)
    assert cookie["httponly"] is True
    assert cookie["samesite"].lower() == "strict"
    assert cookie["path"] == "/"
    assert int(cookie["max-age"]) == SESSION_SECONDS
    # The cookie is a random handle, and the script never sees it in a response body.
    assert len(cookie.value) >= 32
    assert cookie.value not in response.text
    assert cookie.value != response.json()["csrf_token"]


def test_the_session_cookie_is_marked_secure_when_the_backend_is_told_it_is_behind_https(
    harness, officer
):
    client = TestClient(create_app(harness.backend, secure_cookies=True))

    response = client.post(
        SESSION_URL, json={"name": officer.name, "password": officer.password}
    )

    assert session_cookie(response)["secure"] is True


def test_a_wrong_password_and_an_unknown_account_get_the_same_refusal(visitor, officer):
    wrong_password = visitor.sign_in(Account(officer.name, officer.password + "x"))
    unknown_account = visitor.sign_in(Account("nobody", officer.password))

    assert wrong_password.status_code == 401
    assert unknown_account.status_code == 401
    assert wrong_password.json() == unknown_account.json()
    assert "set-cookie" not in wrong_password.headers
    assert "set-cookie" not in unknown_account.headers
    assert visitor.session() == {"signed_in": False}


@pytest.mark.parametrize(
    "body",
    [
        {},
        {"name": "officer1"},
        {"name": "officer1", "password": ""},
        {"name": ["officer1"], "password": {"$ne": ""}},
        {"name": "officer1", "password": "x" * 5000},
        ["officer1", "password"],
    ],
)
def test_a_malformed_sign_in_is_refused(visitor, officer, body):
    response = visitor.client.post(SESSION_URL, json=body)

    assert response.status_code in (401, 422)
    assert visitor.session() == {"signed_in": False}


def test_sign_in_must_be_sent_as_json_which_a_form_on_another_site_cannot_do(visitor, officer):
    as_form = visitor.client.post(
        SESSION_URL, data={"name": officer.name, "password": officer.password}
    )
    as_text = visitor.client.post(
        SESSION_URL,
        content=f'{{"name": "{officer.name}", "password": "{officer.password}"}}',
        headers={"Content-Type": "text/plain"},
    )

    assert as_form.status_code == 415
    assert as_text.status_code == 415
    assert visitor.session() == {"signed_in": False}


def test_sign_in_from_another_site_is_refused(visitor, officer):
    response = visitor.client.post(
        SESSION_URL,
        json={"name": officer.name, "password": officer.password},
        headers={"Origin": "https://elsewhere.example"},
    )

    assert response.status_code == 403
    assert visitor.session() == {"signed_in": False}


def test_each_browser_has_its_own_session(harness, signed_in, officer, second_officer):
    colleague = Browser(harness)
    colleague.sign_in(second_officer)
    bystander = Browser(harness)

    assert signed_in.session()["officer"] == officer.name
    assert colleague.session()["officer"] == second_officer.name
    assert colleague.csrf_token != signed_in.csrf_token
    assert bystander.session() == {"signed_in": False}


def test_signing_out_ends_the_session_on_the_server(harness, signed_in, upstream):
    harness.ingest("alert", upstream.id, alert_level="evacuate")
    (alert,) = signed_in.alerts()
    cookie = signed_in.client.cookies.get("wadiwatch_session")
    token = signed_in.csrf_token

    response = signed_in.sign_out()

    assert response.status_code == 200
    assert signed_in.session() == {"signed_in": False}
    # Someone who kept a copy of the cookie and the token gets nowhere with them.
    thief = Browser(harness)
    thief.client.cookies.set("wadiwatch_session", cookie)
    thief.csrf_token = token
    assert thief.session() == {"signed_in": False}
    assert thief.acknowledge(alert["id"]).status_code == 401


def test_signing_out_needs_the_csrf_token(signed_in, officer):
    response = signed_in.client.delete(SESSION_URL)

    assert response.status_code == 403
    assert signed_in.session()["officer"] == officer.name


def test_a_session_ends_by_itself_after_a_shift(harness, signed_in, officer, clock, upstream):
    harness.ingest("alert", upstream.id, alert_level="warning")
    (alert,) = signed_in.alerts()

    clock.advance(SESSION_SECONDS - 1)
    assert signed_in.session()["officer"] == officer.name

    clock.advance(1)
    assert signed_in.session() == {"signed_in": False}
    assert signed_in.acknowledge(alert["id"]).status_code == 401


def test_a_session_survives_a_restart_of_the_backend(harness, signed_in, officer, upstream):
    harness.restart()

    assert signed_in.session()["officer"] == officer.name
    harness.ingest("alert", upstream.id, alert_level="warning")
    (alert,) = signed_in.alerts()
    assert signed_in.acknowledge(alert["id"]).status_code == 200


def test_signing_in_again_replaces_the_session_of_that_browser(signed_in, officer):
    old_cookie = signed_in.client.cookies.get("wadiwatch_session")
    old_token = signed_in.csrf_token

    signed_in.sign_in(officer)

    assert signed_in.client.cookies.get("wadiwatch_session") != old_cookie
    assert signed_in.csrf_token != old_token
    assert signed_in.session()["officer"] == officer.name


def test_the_password_is_nowhere_in_the_database_files(harness, signed_in, officer):
    harness.stop()

    for path in harness.db_path.parent.iterdir():
        stored = path.read_bytes()
        assert officer.password.encode() not in stored, path.name
        # Neither is the cookie that would let someone take over the session.
        assert signed_in.client.cookies.get("wadiwatch_session").encode() not in stored, path.name


def test_password_hashes_are_salted_slow_to_compute_and_checkable():
    # No account fixture here, so this runs at the cost a real account gets.
    password = "a test password, never used anywhere"

    first, second = hash_password(password), hash_password(password)

    scheme, n, r, p, salt, key = first.split("$")
    assert scheme == "scrypt"
    # No weaker than the OWASP minimum for scrypt.
    assert (int(n), int(r), int(p)) >= (2**15, 8, 3)
    assert first != second
    assert password not in first
    assert verify_password(password, first)
    assert verify_password(password, second)
    assert not verify_password(password + " ", first)
    assert not verify_password(password, "not a stored hash")
