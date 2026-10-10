"""The backend behind its two seams: gateway messages in, API state out."""

import logging
import time
from dataclasses import dataclass

from . import all_clear, health
from .alert_log import ALERT, KINDS as ALERT_LOG_KINDS, build_alert_log
from .contract import ALERT_LEVELS, WADI_NODE, ContractError, parse_message
from .downlink import all_clear_command
from .officers import Officers
from .store import MessageStore

log = logging.getLogger("wadiwatch.backend")

STORED = "stored"
DUPLICATE = "duplicate"
REJECTED = "rejected"

# The period of a curve when the caller does not choose one.
DEFAULT_CURVE_PERIOD_S = 24 * 3600


@dataclass(frozen=True)
class IngestResult:
    status: str
    error: str | None = None


class NotAWadiNode(LookupError):
    """The site has no wadi node with this identifier."""


class EmptyPeriod(ValueError):
    """The period ends before it starts."""


class UnknownAlert(LookupError):
    """No alert entry of the alert log has this id."""


class UnknownWadi(LookupError):
    """The site has no wadi with this identifier."""


class AllClearRefused(Exception):
    """The data does not bear out an all clear. `verdict` says why (all_clear.judge)."""

    def __init__(self, verdict):
        super().__init__(verdict["refusal"])
        self.verdict = verdict


class GatewayUnreachable(Exception):
    """The all clear command could not be handed over on the way to the gateway."""


class Backend:
    def __init__(self, db_path, site, clock=time.time, downlink=None):
        """`downlink`, if given, sends one command towards the gateway.

        It is called with the command as text and returns True once the command
        has been handed over (to the MQTT broker in production). Without one,
        or when it returns False, no all clear can be issued.
        """
        self.site = site
        self._store = MessageStore(db_path)
        self._clock = clock
        self.downlink = downlink
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

    def latest_counter(self, node_id):
        """The counter of the last message stored from a unit, or 0 if there is none."""
        with self._store.reader() as reader:
            message = reader.latest_from(node_id)
        return message["counter"] if message else 0

    # Alert log and officer acknowledgement (ticket #13). The rules are in alert_log.py.

    def alert_log(self, show_officers=False):
        """Every alert, test and all clear, newest first.

        Officers' names are left out unless `show_officers` is set.
        """
        with self._store.reader() as reader:
            messages = reader.in_arrival_order(ALERT_LOG_KINDS)
            commands = reader.commands()
            relayed = reader.commands_relayed()
        return build_alert_log(
            messages,
            self.site,
            self.officers.acknowledgements(),
            show_officers,
            commands={command["command"]: command for command in commands},
            waiting=self._commands_waiting(commands, relayed),
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

    # The all clear (ticket #14). The rule is in all_clear.py, the command in downlink.py.

    def all_clear_verdicts(self):
        """For every wadi: whether an all clear would be accepted now and, if not, why."""
        now = self._clock()
        with self._store.reader() as reader:
            verdicts = [self._judge(reader, wadi, now) for wadi in self.site.wadis]
        return {"now": int(now), "wadis": verdicts}

    def issue_all_clear(self, wadi_id, officer):
        """Send the gateway the all clear for a wadi, on the decision of `officer`.

        Returns the command: {"command", "wadi", "officer", "ts"}. Raises
        UnknownWadi, AllClearRefused when the rule is not met, GatewayUnreachable
        when the command could not be sent. The all clear itself is in force
        only once the gateway has transmitted it and its `all_clear` messages
        have come back: that is what the alert log records.

        A command issued for the same wadi a moment ago and not yet reported
        back by the gateway stands: it is returned, and no second one is sent.
        """
        wadi = self.site.wadi(wadi_id)
        if wadi is None:
            raise UnknownWadi(wadi_id)
        now = self._clock()
        with self._store.reader() as reader:
            waiting = self._commands_waiting(reader.commands(), reader.commands_relayed())
            verdict = self._judge(reader, wadi, now)
        if wadi.id in waiting:
            return waiting[wadi.id]
        if not verdict["allowed"]:
            raise AllClearRefused(verdict)
        if self.downlink is None:
            raise GatewayUnreachable("the backend has no connection to the gateway")

        command = self._store.add_command(wadi.id, officer, now)
        if not self.downlink(all_clear_command(command, int(now), wadi.id)):
            # The row stays, marked as not sent: this number is not used again.
            raise GatewayUnreachable("the command could not be sent to the gateway")
        self._store.mark_command_sent(command)
        log.info("all clear command %d for wadi %d sent by %s", command, wadi.id, officer)
        return {"command": command, "wadi": wadi.id, "officer": officer, "ts": int(now)}

    def _judge(self, reader, wadi, now):
        alert_levels = {
            node.id: self._wadi_node_alert(reader, node)[0] for node in self.site.wadi_nodes
        }
        report = health.describe(reader, self.site, now, list(alert_levels.values()))
        return all_clear.judge(reader, self.site, wadi, now, report["gateway"], alert_levels)

    def _commands_waiting(self, commands, relayed):
        """Wadi id -> the command sent for it that the gateway has not reported back yet.

        A command counts for as long as the gateway would still act on it.
        """
        now = self._clock()
        waiting = {}
        for command in commands:
            fresh = now - command["ts"] < self.site.all_clear_command_valid_s
            if fresh and command["command"] not in relayed:
                waiting[command["wadi"]] = command
        return waiting

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

    # Curves, node health and staleness.

    def readings(self, node_id, start=None, end=None):
        """The curve of one wadi node: its level and rate of rise over a period.

        `start` and `end` are Unix seconds on the gateway's clock, both
        included. The period ends now and lasts a day unless the caller says
        otherwise.
        """
        if self.site.role_of(node_id) != WADI_NODE:
            raise NotAWadiNode(node_id)
        if end is None:
            end = int(self._clock())
        if start is None:
            start = end - DEFAULT_CURVE_PERIOD_S
        if start > end:
            raise EmptyPeriod(f"the period ends ({end}) before it starts ({start})")
        with self._store.reader() as reader:
            readings = reader.readings_between(node_id, start, end)
        return {
            "node": node_id,
            "from": start,
            "to": end,
            "readings": [
                {
                    "ts": reading["ts"],
                    "level_cm": reading["level_cm"],
                    "rise_cm_min": reading["rise_cm_min"],
                }
                for reading in readings
            ],
        }

    def health(self):
        """Every node's health from its latest heartbeat, and how old the gateway's data is."""
        with self._store.reader() as reader:
            wadi_alert_levels = [
                self._wadi_node_alert(reader, node)[0] for node in self.site.wadi_nodes
            ]
            return health.describe(reader, self.site, self._clock(), wadi_alert_levels)

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
            if acknowledgement is None:
                continue
            # What counts is when the alert it answers arrived, if the gateway
            # forwarded that alert: a siren node's acknowledgement of the last
            # refresh can reach the backend just behind the all clear.
            answered = None
            if "alert_counter" in acknowledgement:
                answered = reader.alert_with_counter(wadi_node.id, acknowledgement["alert_counter"])
            arrived = min(acknowledgement["id"], answered["id"]) if answered else acknowledgement["id"]
            if arrived > _in_force_after(reader, wadi_node.id):
                in_force.append(acknowledgement)
        if not in_force:
            return "none", None
        highest = max(in_force, key=lambda ack: ALERT_LEVELS.index(ack["alert_level"]))
        return highest["alert_level"], max(ack["ts"] for ack in in_force)


def _in_force_after(reader, wadi_node_id):
    """Messages about this wadi node's alert count only if they arrived after this row."""
    all_clear = reader.latest_all_clear(wadi_node_id)
    return all_clear["id"] if all_clear else 0
