"""Test support for the alert log, sign-in and acknowledgement.

A `Browser` is one person looking at the dashboard: it has its own cookies, so
a test can have a visitor and several officers at once. Accounts are created
the way the seed step creates them, with passwords generated for the test.

conftest.py loads this module as a plugin, so its fixtures (`visitor`,
`officer`, `second_officer`, `signed_in`) are available to every test.

Hashing a password is slow on purpose (about a third of a second). The account
fixtures turn the cost down through `quick_password_hashing`, so the suite
stays fast; the hash format and every code path are the real ones.
"""

import secrets
from dataclasses import dataclass

import pytest
from fastapi.testclient import TestClient

from wadiwatch_backend import officers
from wadiwatch_backend.seed import create_accounts

ALERT_LOG_URL = "/api/alert-log"
SESSION_URL = "/api/session"
CSRF_HEADER = "X-CSRF-Token"


@dataclass(frozen=True)
class Account:
    name: str
    password: str


def new_account(harness, name):
    """An officer account in the harness database, with a password made up for this test."""
    account = Account(name, secrets.token_urlsafe(16))
    create_accounts(harness.db_path, {account.name: account.password}, clock=harness.clock)
    return account


def acknowledgement_url(alert_id):
    return f"/api/alerts/{alert_id}/acknowledgement"


def all_clear_url(wadi_id):
    return f"/api/wadis/{wadi_id}/all-clear"


class Browser:
    """One person's browser. It keeps its cookies when the backend restarts."""

    def __init__(self, harness):
        self._harness = harness
        self._client = None
        # What the dashboard script keeps in memory after sign-in.
        self.csrf_token = None

    @property
    def client(self):
        app = self._harness.client.app
        if self._client is None or self._client.app is not app:
            cookies = dict(self._client.cookies) if self._client is not None else None
            self._client = TestClient(app, cookies=cookies)
        return self._client

    def sign_in(self, account):
        response = self.client.post(
            SESSION_URL, json={"name": account.name, "password": account.password}
        )
        if response.status_code == 200:
            self.csrf_token = response.json()["csrf_token"]
        return response

    def sign_out(self):
        return self.client.delete(SESSION_URL, headers=self._csrf_headers())

    def session(self):
        response = self.client.get(SESSION_URL)
        assert response.status_code == 200, response.text
        return response.json()

    def alert_log(self):
        """The entries of the alert log, newest first."""
        response = self.client.get(ALERT_LOG_URL)
        assert response.status_code == 200, response.text
        return response.json()["entries"]

    def alerts(self):
        return [entry for entry in self.alert_log() if entry["type"] == "alert"]

    def acknowledge(self, alert_id, headers=None):
        """Ask to acknowledge an alert, as the dashboard's Acknowledge button does."""
        if headers is None:
            headers = self._csrf_headers()
        return self.client.post(acknowledgement_url(alert_id), headers=headers)

    def issue_all_clear(self, wadi_id, headers=None):
        """Ask for the all clear for a wadi, as the dashboard's All clear button does."""
        if headers is None:
            headers = self._csrf_headers()
        return self.client.post(all_clear_url(wadi_id), headers=headers)

    def all_clears(self):
        return [entry for entry in self.alert_log() if entry["type"] == "all_clear"]

    def _csrf_headers(self):
        return {CSRF_HEADER: self.csrf_token} if self.csrf_token else {}


@pytest.fixture
def quick_password_hashing(monkeypatch):
    """Hash passwords at a token cost for the length of one test."""
    monkeypatch.setattr(officers, "SCRYPT_N", 2**8)
    monkeypatch.setattr(officers, "SCRYPT_P", 1)
    # The stand-in hash checked for unknown accounts is made at the cost in force.
    monkeypatch.setattr(officers, "_unknown_account_hash", None)


@pytest.fixture
def visitor(harness):
    """Someone who has not signed in."""
    return Browser(harness)


@pytest.fixture
def officer(harness, quick_password_hashing):
    return new_account(harness, "officer1")


@pytest.fixture
def second_officer(harness, quick_password_hashing):
    return new_account(harness, "officer2")


@pytest.fixture
def signed_in(harness, officer):
    """The browser of `officer`, signed in."""
    browser = Browser(harness)
    assert browser.sign_in(officer).status_code == 200
    return browser
