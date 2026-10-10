"""The backend behind its two seams: gateway messages in, API state out."""

import logging
import time
from dataclasses import dataclass

from .alert_log import ALERT, KINDS as ALERT_LOG_KINDS, build_alert_log
from .contract import ALERT_LEVELS, WADI_NODE, ContractError, parse_message
from .officers import Officers
from .store import MessageStore

log = logging.getLogger("wadiwatch.backend")

STORED = "stored"
DUPLICATE = "duplicate"
REJECTED = "rejected"


@dataclass(frozen=True)
class IngestResult:
    status: str
    error: str | None = None


class UnknownAlert(LookupError):
    """No alert entry of the alert log has this id."""


class Backend:
    def __init__(self, db_path, site, clock=time.time):
        self.site = site
        self._store = MessageStore(db_path)
        self._clock = clock
        # Accounts, sessions and officer acknowledgements (ticket #13).
        self.officers = Officers(db_path, clock)

    def close(self):
        self._store.close()
        self.officers.close()

    def ingest(self, payload):
        """Take one gateway message exactly as it came off the uplink.

        Every transport (MQTT, HTTP, tests) calls this and nothing else. A
        malformed message is rejected whole; a message already stored is
        ignored, so the gateway may send again when it is unsure.
        """
        try:
            message = parse_message(payload, self.site)
        except ContractError as error:
            log.warning("rejected gateway message: %s", error)
            return IngestResult(REJECTED, str(error))
        if self._store.add(message, received_at=self._clock()):
            return IngestResult(STORED)
        return IngestResult(DUPLICATE)

    # Alert log and officer acknowledgement (ticket #13). The rules are in alert_log.py.

    def alert_log(self, show_officers=False):
        """Every alert, test and all clear, newest first.

        Officers' names are left out unless `show_officers` is set.
        """
        with self._store.reader() as reader:
            messages = reader.in_arrival_order(ALERT_LOG_KINDS)
        return build_alert_log(
            messages, self.site, self.officers.acknowledgements(), show_officers
        )

    def acknowledge_alert(self, alert_id, officer):
        """Record that `officer` (an account name) took charge of this alert.

        `alert_id` is the `id` of an alert entry of the alert log. Raises
        UnknownAlert for anything else. The first acknowledgement stands.
        """
        if not any(
            entry["id"] == alert_id and entry["type"] == ALERT for entry in self.alert_log()
        ):
            raise UnknownAlert(alert_id)
        return self.officers.acknowledge(alert_id, officer)

    def nodes(self):
        """Every configured node with its latest level and current alert level."""
        with self._store.reader() as reader:
            nodes = [self._describe(reader, node) for node in self.site.nodes]
        gateway = self.site.gateway
        return {
            "site": {
                "name": self.site.name,
                "positions_are_placeholders": self.site.positions_are_placeholders,
            },
            "gateway": {
                "id": gateway.id,
                "name": gateway.name,
                "lat": gateway.lat,
                "lon": gateway.lon,
            },
            "nodes": nodes,
        }

    def _describe(self, reader, node):
        if node.role == WADI_NODE:
            alert_level, alert_ts = self._wadi_node_alert(reader, node)
        else:
            alert_level, alert_ts = self._siren_node_alert(reader, node)
        reading = reader.latest_from(node.id, "reading") or {}
        last_message = reader.latest_from(node.id) or {}
        return {
            "id": node.id,
            "role": node.role,
            "name": node.name,
            "lat": node.lat,
            "lon": node.lon,
            "upstream": node.upstream,
            "alert_level": alert_level,
            "alert_ts": alert_ts,
            "level_cm": reading.get("level_cm"),
            "rise_cm_min": reading.get("rise_cm_min"),
            "level_ts": reading.get("ts"),
            "last_message_ts": last_message.get("ts"),
        }

    @staticmethod
    def _wadi_node_alert(reader, node):
        """The level of the node's latest alert, unless an all clear came after it."""
        alert = reader.latest_from(node.id, "alert")
        if alert is None or _in_force_after(reader, node.id) > alert["id"]:
            return "none", None
        if alert["alert_level"] == "none":
            return "none", None
        return alert["alert_level"], alert["ts"]

    def _siren_node_alert(self, reader, node):
        """The highest level this siren node acknowledged and has not been cleared of."""
        in_force = []
        for wadi_node in self.site.wadi_nodes:
            acknowledgement = reader.latest_acknowledgement(node.id, wadi_node.id)
            if acknowledgement and acknowledgement["id"] > _in_force_after(reader, wadi_node.id):
                in_force.append(acknowledgement)
        if not in_force:
            return "none", None
        highest = max(in_force, key=lambda ack: ALERT_LEVELS.index(ack["alert_level"]))
        return highest["alert_level"], max(ack["ts"] for ack in in_force)


def _in_force_after(reader, wadi_node_id):
    """Messages about this wadi node's alert count only if they arrived after this row."""
    all_clear = reader.latest_all_clear(wadi_node_id)
    return all_clear["id"] if all_clear else 0
