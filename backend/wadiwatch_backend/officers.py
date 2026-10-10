"""Officer accounts, their sessions, and the alerts officers have acknowledged.

These live in the same SQLite file as the gateway messages, in tables of their
own and through a connection of their own, so the seed step can create accounts
while the backend is running.

What is kept, and what is not:

- A password is kept only as a salted scrypt hash. The password itself is never
  stored or logged.
- A session is a random token held in an HttpOnly cookie. Only its SHA-256 is
  stored, so a copy of the database does not hand out live sessions.
- Each session has its own CSRF token, which the dashboard script must send
  back in a header with every request that changes something.
"""

import base64
import binascii
import hashlib
import hmac
import re
import secrets
import sqlite3
import threading
import time
from dataclasses import dataclass
from pathlib import Path

# A session lasts one long duty shift, then the officer signs in again.
SESSION_SECONDS = 12 * 3600

MIN_PASSWORD_LENGTH = 12
MAX_PASSWORD_LENGTH = 128
# Account names are shown in the alert log: short, lower case, no spaces.
NAME_PATTERN = re.compile(r"[a-z0-9][a-z0-9._-]{0,31}")

# scrypt cost, from the OWASP password storage guidance (N=2^15, r=8, p=3).
# The parameters are stored with each hash, so they can be raised later
# without invalidating existing accounts.
SCRYPT_N = 2**15
SCRYPT_R = 8
SCRYPT_P = 3
SALT_BYTES = 16
KEY_BYTES = 32
# Refuse to spend more than this on a stored hash, whatever it claims to need.
SCRYPT_MAX_MEMORY = 128 * 1024 * 1024

SCHEMA = """
CREATE TABLE IF NOT EXISTS officers (
    name          TEXT PRIMARY KEY,
    password_hash TEXT NOT NULL,
    created_at    REAL NOT NULL
);
CREATE TABLE IF NOT EXISTS sessions (
    token_hash TEXT PRIMARY KEY,
    officer    TEXT NOT NULL,
    csrf_token TEXT NOT NULL,
    expires_at REAL NOT NULL
);
-- alert_id is the alert log entry: messages.id of the alert message that opened it.
CREATE TABLE IF NOT EXISTS officer_acknowledgements (
    alert_id        INTEGER PRIMARY KEY,
    officer         TEXT    NOT NULL,
    acknowledged_at INTEGER NOT NULL
);
"""


def _encode(raw):
    return base64.b64encode(raw).decode("ascii")


def _scrypt(password, salt, n, r, p, length):
    return hashlib.scrypt(
        password.encode("utf-8"),
        salt=salt,
        n=n,
        r=r,
        p=p,
        dklen=length,
        maxmem=SCRYPT_MAX_MEMORY,
    )


def hash_password(password):
    """A salted hash of the password, with the parameters needed to check it later."""
    salt = secrets.token_bytes(SALT_BYTES)
    key = _scrypt(password, salt, SCRYPT_N, SCRYPT_R, SCRYPT_P, KEY_BYTES)
    return f"scrypt${SCRYPT_N}${SCRYPT_R}${SCRYPT_P}${_encode(salt)}${_encode(key)}"


def verify_password(password, stored):
    try:
        scheme, n, r, p, salt, key = stored.split("$")
        if scheme != "scrypt":
            return False
        salt = base64.b64decode(salt, validate=True)
        key = base64.b64decode(key, validate=True)
        candidate = _scrypt(password, salt, int(n), int(r), int(p), len(key))
    except (ValueError, binascii.Error):
        return False
    return hmac.compare_digest(candidate, key)


_unknown_account_hash = None


def _spend_time_on_unknown_account(password):
    """Do the work of a real check, so an unknown name answers no faster than a wrong password."""
    global _unknown_account_hash
    if _unknown_account_hash is None:
        _unknown_account_hash = hash_password(secrets.token_urlsafe(16))
    verify_password(password, _unknown_account_hash)


def _token_hash(token):
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


@dataclass(frozen=True)
class Session:
    officer: str
    csrf_token: str


@dataclass(frozen=True)
class OfficerAcknowledgement:
    officer: str
    ts: int


class Officers:
    def __init__(self, db_path, clock=time.time):
        path = Path(db_path)
        path.parent.mkdir(parents=True, exist_ok=True)
        self._clock = clock
        self._lock = threading.Lock()
        self._connection = sqlite3.connect(path, check_same_thread=False, timeout=10)
        with self._lock, self._connection:
            self._connection.execute("PRAGMA journal_mode=WAL")
            self._connection.executescript(SCHEMA)

    def close(self):
        with self._lock:
            self._connection.close()

    # Accounts

    @staticmethod
    def problem_with(name, password):
        """Why this account cannot be created, or None. The answer never repeats the password."""
        if not isinstance(name, str) or not NAME_PATTERN.fullmatch(name):
            return (
                "an account name is 1 to 32 characters: lower-case letters, digits, "
                "dot, dash or underscore, starting with a letter or digit"
            )
        if not isinstance(password, str):
            return "the password must be text"
        if len(password) < MIN_PASSWORD_LENGTH:
            return f"the password is too short (at least {MIN_PASSWORD_LENGTH} characters)"
        if len(password) > MAX_PASSWORD_LENGTH:
            return f"the password is too long (at most {MAX_PASSWORD_LENGTH} characters)"
        return None

    def add(self, name, password):
        """Create the account, or give an existing account this password.

        A changed password ends the account's sessions.
        """
        problem = self.problem_with(name, password)
        if problem:
            raise ValueError(problem)
        with self._lock:
            row = self._connection.execute(
                "SELECT password_hash FROM officers WHERE name = ?", (name,)
            ).fetchone()
        if row is not None and verify_password(password, row[0]):
            return
        password_hash = hash_password(password)
        with self._lock, self._connection:
            self._connection.execute(
                "INSERT INTO officers (name, password_hash, created_at) VALUES (?, ?, ?)"
                " ON CONFLICT (name) DO UPDATE SET password_hash = excluded.password_hash",
                (name, password_hash, self._clock()),
            )
            self._connection.execute("DELETE FROM sessions WHERE officer = ?", (name,))

    # Sessions

    def sign_in(self, name, password):
        """(session token, Session) for a correct name and password, otherwise None."""
        if not isinstance(name, str) or not isinstance(password, str):
            return None
        if not password or len(password) > MAX_PASSWORD_LENGTH:
            return None
        with self._lock:
            row = self._connection.execute(
                "SELECT password_hash FROM officers WHERE name = ?", (name,)
            ).fetchone()
        if row is None:
            _spend_time_on_unknown_account(password)
            return None
        if not verify_password(password, row[0]):
            return None
        token = secrets.token_urlsafe(32)
        session = Session(officer=name, csrf_token=secrets.token_urlsafe(32))
        now = self._clock()
        with self._lock, self._connection:
            self._connection.execute("DELETE FROM sessions WHERE expires_at <= ?", (now,))
            self._connection.execute(
                "INSERT INTO sessions (token_hash, officer, csrf_token, expires_at)"
                " VALUES (?, ?, ?, ?)",
                (_token_hash(token), name, session.csrf_token, now + SESSION_SECONDS),
            )
        return token, session

    def session(self, token):
        """The session this cookie value stands for, if it is still running."""
        if not token:
            return None
        with self._lock:
            row = self._connection.execute(
                "SELECT officer, csrf_token FROM sessions WHERE token_hash = ? AND expires_at > ?",
                (_token_hash(token), self._clock()),
            ).fetchone()
        return Session(officer=row[0], csrf_token=row[1]) if row else None

    def sign_out(self, token):
        if not token:
            return
        with self._lock, self._connection:
            self._connection.execute(
                "DELETE FROM sessions WHERE token_hash = ?", (_token_hash(token),)
            )

    # Acknowledgement of alerts

    def acknowledge(self, alert_id, officer):
        """Record that this officer acknowledged the alert. The first acknowledgement stands."""
        with self._lock, self._connection:
            self._connection.execute(
                "INSERT INTO officer_acknowledgements (alert_id, officer, acknowledged_at)"
                " VALUES (?, ?, ?) ON CONFLICT (alert_id) DO NOTHING",
                (alert_id, officer, int(self._clock())),
            )
            row = self._connection.execute(
                "SELECT officer, acknowledged_at FROM officer_acknowledgements WHERE alert_id = ?",
                (alert_id,),
            ).fetchone()
        return OfficerAcknowledgement(officer=row[0], ts=row[1])

    def acknowledgements(self):
        """Alert log entry id -> OfficerAcknowledgement, for every acknowledged alert."""
        with self._lock:
            rows = self._connection.execute(
                "SELECT alert_id, officer, acknowledged_at FROM officer_acknowledgements"
            ).fetchall()
        return {row[0]: OfficerAcknowledgement(officer=row[1], ts=row[2]) for row in rows}
