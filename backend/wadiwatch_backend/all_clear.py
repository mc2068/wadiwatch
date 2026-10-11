"""When an all clear may be issued for a wadi: the rule behind the officer's control.

The all clear is the officer's decision, but the backend refuses it unless the
data bears it out: at every wadi node of the wadi the level has stayed below
that node's Watch threshold for the quiet period (30 minutes on the prototype
site).

"Has stayed below" needs evidence, and silence is not evidence:

- The gateway's data must be current. If the gateway has gone silent (the
  stale-data rule of health.py), nothing here says how the wadi is now, and
  the all clear is refused whatever the last levels were.
- Every wadi node must have reported a level, and recently enough. A node at
  Warning or above measures and reports every minute, so two missed alert
  refresh intervals make it silent. Any other node may be in dry mode, where
  it reports its level only in its hourly heartbeat, so it is silent after two
  missed heartbeat intervals. A wadi node in dry mode is one that has found
  its level below the Watch threshold: it goes to storm mode, and reports, as
  soon as it does not.
- A level must have been measured. A wadi node whose sensor gives no usable
  reading keeps the last level it measured and goes on sending it, in every
  alert refresh and heartbeat, marked as repeated (`level_repeated`). Such a
  report proves that the node is alive and nothing about the water: while it
  is the latest the all clear is refused, and the time the node could not
  measure does not count towards the quiet period.
- The time below the threshold counts from the first level measured below it
  after the last report that was at or above it or was not measured, on the
  gateway's clock (`ts`), so levels forwarded late after an uplink outage count
  for when they were measured.

A level is whatever a wadi node reported: in a reading, behind an alert, or in
a heartbeat.
"""

from .health import MISSED_INTERVALS, REFRESHED_ALERT_LEVELS

# Why a wadi node stands in the way of the all clear.
NO_LEVEL = "no_level"
SILENT = "silent"
NOT_MEASURING = "not_measuring"
AT_OR_ABOVE_WATCH = "at_or_above_watch"
BELOW_WATCH_TOO_RECENTLY = "below_watch_too_recently"

# Why a wadi cannot be given the all clear.
STALE = "stale"
WADI_NODES = "wadi_nodes"


def judge(reader, site, wadi, now, gateway, alert_levels):
    """Whether an all clear for `wadi` would be accepted now, and if not, why.

    `gateway` is the gateway part of the health report (is its data stale, how
    old is it); `alert_levels` maps each wadi node's id to its alert level now.
    """
    nodes = [
        _judge_node(reader, site, node, now, alert_levels.get(node.id, "none"))
        for node in site.wadi_nodes_of(wadi.id)
    ]
    if gateway["stale"]:
        reason = STALE
        refusal = _stale_refusal(gateway)
    elif any(node["reason"] for node in nodes):
        reason = WADI_NODES
        refusal = " ".join(node["refusal"] for node in nodes if node["reason"])
    else:
        reason = None
        refusal = None
    return {
        "wadi": {"id": wadi.id, "name": wadi.name},
        "allowed": reason is None,
        "reason": reason,
        "refusal": refusal,
        "quiet_s": site.all_clear_quiet_s,
        "nodes": nodes,
    }


def _judge_node(reader, site, node, now, alert_level):
    watch_cm = node.watch_level_cm
    # The latest report that carries a level, measured or repeated.
    latest = None
    # When the node last measured a level.
    measured_ts = None
    below_since = None
    # Whether the reports looked at so far, newest first, are all of a level
    # measured below the threshold.
    unbroken = True
    for ts, level_cm, measured in reader.level_reports(node.id):
        if latest is None:
            latest = (ts, level_cm, measured)
        if measured and measured_ts is None:
            measured_ts = ts
        if not measured or level_cm >= watch_cm:
            unbroken = False
        if unbroken:
            below_since = ts
        elif measured_ts is not None:
            break

    judged = {
        "id": node.id,
        "name": node.name,
        "watch_level_cm": watch_cm,
        "level_cm": None if latest is None else latest[1],
        "level_ts": None if latest is None else latest[0],
        # When the node last measured a level; earlier than level_ts while it only
        # repeats one, None if it has never measured.
        "measured_ts": measured_ts,
        # Since when the level has been below the Watch threshold, and from when
        # that is long enough. Both None unless the latest level is below it.
        "below_since": None,
        "allowed_from": None,
        "reason": None,
        "refusal": None,
    }

    if latest is None:
        judged["reason"] = NO_LEVEL
        judged["refusal"] = f"{node.name} has not reported a water level."
        return judged

    level_ts, level_cm, measured = latest
    if alert_level in REFRESHED_ALERT_LEVELS:
        expected_interval_s = site.alert_refresh_interval_s
    else:
        expected_interval_s = site.heartbeat_interval_s
    if now - level_ts > MISSED_INTERVALS * expected_interval_s:
        judged["reason"] = SILENT
        judged["refusal"] = (
            f"{node.name} last reported its level {_duration(now - level_ts)} ago, "
            "which is too long ago to rely on."
        )
        return judged

    if not measured:
        judged["reason"] = NOT_MEASURING
        if measured_ts is None:
            since = "has never measured its water level"
        else:
            since = f"has not measured its water level for {_duration(now - measured_ts)}"
        judged["refusal"] = (
            f"{node.name} {since}: its level sensor gives no usable reading, and the "
            "level it reports is an old one."
        )
        return judged

    if level_cm >= watch_cm:
        judged["reason"] = AT_OR_ABOVE_WATCH
        judged["refusal"] = (
            f"{node.name} is at {level_cm:g} cm, at or above its Watch threshold "
            f"of {watch_cm:g} cm."
        )
        return judged

    judged["below_since"] = below_since
    judged["allowed_from"] = below_since + site.all_clear_quiet_s
    if now < judged["allowed_from"]:
        judged["reason"] = BELOW_WATCH_TOO_RECENTLY
        judged["refusal"] = (
            f"{node.name} has been below its Watch threshold for "
            f"{_duration(max(0, now - below_since))}; {_duration(site.all_clear_quiet_s)} "
            "are required."
        )
    return judged


def _stale_refusal(gateway):
    if gateway["age_s"] is None:
        heard = "Nothing has been received from the gateway."
    else:
        heard = f"The gateway's latest data is {_duration(gateway['age_s'])} old."
    return heard + " An all clear is not issued on an old picture of the wadi."


def _duration(seconds):
    """A length of time in words, to the minute: "less than a minute", "12 min", "2 h 5 min"."""
    minutes = int(seconds) // 60
    if minutes < 1:
        return "less than a minute"
    if minutes < 60:
        return f"{minutes} min"
    hours, minutes = divmod(minutes, 60)
    return f"{hours} h {minutes} min" if minutes else f"{hours} h"
