"""SQLite storage: one row per accepted gateway message, in the order it arrived."""

import json
import sqlite3
import threading
from contextlib import contextmanager
from pathlib import Path

# `body` keeps the whole message. The other columns repeat the fields that
# queries filter or sort on, so a later ticket can read any contract field
# from `body` without a schema change.
SCHEMA = """
CREATE TABLE IF NOT EXISTS messages (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    received_at REAL    NOT NULL,
    ts          INTEGER NOT NULL,
    kind        TEXT    NOT NULL,
    node        INTEGER NOT NULL,
    counter     INTEGER NOT NULL,
    wadi_node   INTEGER,
    body        TEXT    NOT NULL,
    UNIQUE (node, counter, ts)
);
CREATE INDEX IF NOT EXISTS messages_by_node_kind ON messages (node, kind, id);
CREATE INDEX IF NOT EXISTS messages_by_kind_wadi_node ON messages (kind, wadi_node, id);
"""


class MessageStore:
    """One connection shared by every thread, one caller at a time."""

    def __init__(self, path):
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()
        self._connection = sqlite3.connect(path, check_same_thread=False)
        with self._lock, self._connection:
            self._connection.execute("PRAGMA journal_mode=WAL")
            self._connection.executescript(SCHEMA)

    def close(self):
        with self._lock:
            self._connection.close()

    def add(self, message, received_at):
        """Store a validated message. Returns False if it was already stored."""
        with self._lock, self._connection:
            cursor = self._connection.execute(
                "INSERT INTO messages (received_at, ts, kind, node, counter, wadi_node, body)"
                " VALUES (?, ?, ?, ?, ?, ?, ?)"
                " ON CONFLICT (node, counter, ts) DO NOTHING",
                (
                    received_at,
                    message["ts"],
                    message["kind"],
                    message["node"],
                    message["counter"],
                    message.get("wadi_node"),
                    json.dumps(message, separators=(",", ":")),
                ),
            )
            return cursor.rowcount == 1

    @contextmanager
    def reader(self):
        """A consistent view for a group of queries: nothing is written meanwhile."""
        with self._lock:
            yield Reader(self._connection)


class Reader:
    def __init__(self, connection):
        self._connection = connection

    def _latest(self, where, parameters):
        row = self._connection.execute(
            f"SELECT id, body FROM messages WHERE {where} ORDER BY id DESC LIMIT 1", parameters
        ).fetchone()
        if row is None:
            return None
        return dict(json.loads(row[1]), id=row[0])

    def latest_from(self, node, kind=None):
        """The last message this node sent, optionally of one kind."""
        if kind is None:
            return self._latest("node = ?", (node,))
        return self._latest("node = ? AND kind = ?", (node, kind))

    # Curves, node health and staleness.

    def readings_between(self, node, start, end):
        """The readings of a wadi node the gateway stamped from `start` to `end`, oldest first."""
        rows = self._connection.execute(
            "SELECT body FROM messages WHERE node = ? AND kind = 'reading' AND ts BETWEEN ? AND ?"
            " ORDER BY ts, id",
            (node, start, end),
        )
        return [json.loads(body) for (body,) in rows]

    def first_ts(self):
        """Gateway time of the first message ever stored, or None."""
        row = self._connection.execute("SELECT ts FROM messages ORDER BY id LIMIT 1").fetchone()
        return row[0] if row else None

    def last_received_at(self):
        """Backend time at which the latest message arrived, or None."""
        row = self._connection.execute(
            "SELECT received_at FROM messages ORDER BY id DESC LIMIT 1"
        ).fetchone()
        return row[0] if row else None

    # Alert levels.

    def latest_all_clear(self, wadi_node):
        return self._latest("kind = 'all_clear' AND wadi_node = ?", (wadi_node,))

    def latest_acknowledgement(self, siren_node, wadi_node):
        return self._latest(
            "kind = 'acknowledgement' AND node = ? AND wadi_node = ?", (siren_node, wadi_node)
        )
