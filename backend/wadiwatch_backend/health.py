"""Node health and staleness: is every node alive, and is the picture current?

Two questions with two clocks:

- A node is faulty when its heartbeat is missing. That is judged on the time
  the gateway heard the heartbeat (`ts`), so a heartbeat forwarded late after
  an uplink outage is as old as it really is.
- The data is stale when the gateway itself has gone silent. That is judged on
  the time the backend received its latest message (`received_at`), whatever
  the message says about when it was measured.

How long a silence is still normal follows from what the wadi nodes send
(`report_interval_s`): the more a node has to say, the sooner its silence means
something.
"""

from .contract import ALERT_LEVELS

# The specification's rule: missing for more than two intervals is a fault.
MISSED_INTERVALS = 2

# A wadi node at one of these alert levels repeats its alert every refresh interval.
REFRESHED_ALERT_LEVELS = ALERT_LEVELS[ALERT_LEVELS.index("warning") :]


def report_interval_s(site, alert_level):
    """How often, at the least, a wadi node at this alert level is heard from.

    - At Warning or Evacuate it sends its alert refresh, with its level, every
      alert refresh interval.
    - At Watch it has no alert refresh and may be back in dry mode, but a node
      that holds an alert level sends a reading with every measurement: every
      dry-mode interval at the least.
    - With no alert level it may be in dry mode, where it sends nothing but its
      heartbeat, which carries its last level.
    """
    if alert_level in REFRESHED_ALERT_LEVELS:
        return site.alert_refresh_interval_s
    if alert_level != "none":
        return site.dry_mode_interval_s
    return site.heartbeat_interval_s

# What a heartbeat says about the node, as shown in its health. The last two
# are optional in the contract: a wadi node counts discards, a siren node
# reports lost alert refreshes.
HEARTBEAT_FIELDS = ("battery_mv", "rssi_dbm", "low_battery", "tamper", "discards", "refresh_lost")


def describe(reader, site, now, wadi_alert_levels):
    """The health of every configured node and the age of the gateway's latest data.

    `wadi_alert_levels` are the alert levels now in force at the wadi nodes;
    they decide how long a silence from the gateway is still normal.
    """
    faulty_after_s = MISSED_INTERVALS * site.heartbeat_interval_s
    heard_since = reader.first_ts()
    return {
        "now": int(now),
        "heartbeat_interval_s": site.heartbeat_interval_s,
        "faulty_after_s": faulty_after_s,
        "gateway": _gateway(reader, site, now, wadi_alert_levels),
        "nodes": [
            _node(reader, node, now, faulty_after_s, heard_since) for node in site.nodes
        ],
    }


def _node(reader, node, now, faulty_after_s, heard_since):
    heartbeat = reader.latest_from(node.id, "heartbeat")
    described = {"id": node.id, "role": node.role, "name": node.name}
    if heartbeat is None:
        # Never heard: a fault once the rest of the site has been heard for long
        # enough that this node's heartbeat should have come too.
        silent_since = heard_since
        described.update(heartbeat_ts=None, heartbeat_age_s=None)
        described.update(dict.fromkeys(HEARTBEAT_FIELDS))
    else:
        silent_since = heartbeat["ts"]
        described.update(heartbeat_ts=heartbeat["ts"], heartbeat_age_s=_age(now, heartbeat["ts"]))
        described.update({name: heartbeat.get(name) for name in HEARTBEAT_FIELDS})
    described["faulty"] = silent_since is not None and now - silent_since > faulty_after_s
    return described


def _gateway(reader, site, now, wadi_alert_levels):
    # The gateway forwards what it hears, so it is heard from as often as the wadi
    # node with the most to say: every heartbeat interval on a dry day, when the
    # nodes send nothing but their heartbeats.
    expected_interval_s = min(
        [report_interval_s(site, level) for level in wadi_alert_levels]
        + [site.heartbeat_interval_s]
    )
    stale_after_s = MISSED_INTERVALS * expected_interval_s
    last_received_at = reader.last_received_at()
    return {
        "id": site.gateway.id,
        "name": site.gateway.name,
        "last_received_at": None if last_received_at is None else int(last_received_at),
        "age_s": None if last_received_at is None else _age(now, last_received_at),
        "stale_after_s": stale_after_s,
        # Nothing received at all is no picture, so it is not a current one either.
        "stale": last_received_at is None or now - last_received_at > stale_after_s,
    }


def _age(now, then):
    """Whole seconds since `then`; never negative if the two clocks disagree slightly."""
    return max(0, int(now - then))
