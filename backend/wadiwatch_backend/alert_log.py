"""The alert log: stored gateway messages read as alerts, tests and all clears.

Nothing is stored for the log itself. It is worked out from the messages each
time it is asked for, so it cannot drift from them.

- An **alert** entry is one change of alert level at one wadi node. The refresh
  a node sends every minute at Warning and above belongs to the entry it
  repeats. The entry's `id` is the stored id of the alert message that opened
  it; officers acknowledge an alert by that id.
- A **test** entry is a siren node's local test.
- An **all clear** entry ends the alert of one wadi node. The next alert from
  that node opens a new entry, even at the same level. The gateway reports
  every all clear packet it transmits, and it transmits one again for a wadi
  node that has not heard: a repeat for the same command, with no alert from
  that node in between, belongs to the entry it repeats. The entry says which
  officer issued the all clear and when, if the gateway's message carries the
  number of a command this backend sent.

A siren node's acknowledgement is recorded against the alert it answers: the
entry holding the alert message whose counter it carries or, if it carries
none (or the gateway missed that alert message), the wadi node's alert in
force at the level it names.
"""

# The message kinds the log is built from.
KINDS = ("alert", "acknowledgement", "all_clear", "test")

ALERT = "alert"
TEST = "test"
ALL_CLEAR = "all_clear"

# The dashboard keeps asking for an officer's acknowledgement at this level.
ACKNOWLEDGEMENT_REQUESTED_AT = "evacuate"


def build_alert_log(
    messages, site, acknowledgements=None, show_officers=False, commands=None, waiting=None
):
    """The alert log, newest entry first.

    `messages` are the stored messages of `KINDS` in arrival order.
    `acknowledgements` maps an alert entry id to the officer's acknowledgement.
    `commands` maps the number of an all clear command to the command
    ({"officer", "ts", ...}); `waiting` maps a wadi id to the command sent for
    it that the gateway has not reported back yet.
    An officer's name is included only if `show_officers` is set: the log is
    public, the names are for signed-in officers.
    """
    acknowledgements = acknowledgements or {}
    commands = commands or {}
    waiting = waiting or {}

    def by_officer(ts, officer):
        """Who did something and when, as far as this reader may know."""
        return {"ts": ts, "officer": officer} if show_officers else {"ts": ts}

    names = {node.id: node.name for node in site.nodes}
    names[site.gateway.id] = site.gateway.name

    entries = []
    # Wadi node id -> its alert entry in force.
    in_force = {}
    # (wadi node id, counter of an alert message) -> the entry that message belongs to.
    by_counter = {}
    # Entry id -> {siren node id: time of its first acknowledgement}.
    answered = {}
    # Wadi node id -> its latest all clear entry, while no alert has followed it.
    cleared = {}

    def entry(message, entry_type, **fields):
        made = {
            "id": message["id"],
            "type": entry_type,
            "ts": message["ts"],
            "node": message["node"],
            "node_name": names.get(message["node"]),
            **fields,
        }
        entries.append(made)
        return made

    for message in messages:
        kind = message["kind"]
        if kind == "alert":
            node = message["node"]
            if message["alert_level"] == "none":
                in_force.pop(node, None)
                continue
            cleared.pop(node, None)
            current = in_force.get(node)
            if current is None or current["alert_level"] != message["alert_level"]:
                current = entry(message, ALERT, alert_level=message["alert_level"])
                in_force[node] = current
                answered[current["id"]] = {}
            by_counter[(node, message["counter"])] = current
        elif kind == "acknowledgement":
            wadi_node = message["wadi_node"]
            alert = by_counter.get((wadi_node, message.get("alert_counter")))
            if alert is None:
                alert = in_force.get(wadi_node)
                if alert is None or alert["alert_level"] != message["alert_level"]:
                    continue
            answered[alert["id"]].setdefault(message["node"], message["ts"])
        elif kind == "all_clear":
            wadi_node = message["wadi_node"]
            command = message.get("command")
            in_force.pop(wadi_node, None)
            before = cleared.get(wadi_node)
            if before is not None and command is not None and before["command"] == command:
                continue
            issued = commands.get(command)
            cleared[wadi_node] = entry(
                message,
                ALL_CLEAR,
                wadi_node=wadi_node,
                wadi_node_name=names.get(wadi_node),
                command=command,
                issued=None if issued is None else by_officer(issued["ts"], issued["officer"]),
            )
        elif kind == "test":
            entry(message, TEST)

    for made in entries:
        if made["type"] != ALERT:
            continue
        made["in_force"] = in_force.get(made["node"]) is made
        wadi = site.wadi_of(made["node"])
        made["wadi"] = None if wadi is None else {"id": wadi.id, "name": wadi.name}
        # An all clear for the wadi has been sent to the gateway and is on its way.
        sent = waiting.get(wadi.id) if wadi is not None and made["in_force"] else None
        made["all_clear_sent"] = None if sent is None else by_officer(sent["ts"], sent["officer"])
        made["siren_nodes"] = [
            {
                "id": siren.id,
                "name": siren.name,
                "acknowledged_ts": answered[made["id"]].get(siren.id),
            }
            for siren in site.siren_nodes
        ]
        acknowledgement = acknowledgements.get(made["id"])
        if acknowledgement is None:
            made["acknowledgement"] = None
        else:
            made["acknowledgement"] = by_officer(acknowledgement.ts, acknowledgement.officer)
        made["acknowledgement_requested"] = (
            made["in_force"]
            and made["alert_level"] == ACKNOWLEDGEMENT_REQUESTED_AT
            and acknowledgement is None
        )

    entries.reverse()
    return entries
