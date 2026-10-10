"""The alert log: stored gateway messages read as alerts, tests and all clears.

Nothing is stored for the log itself. It is worked out from the messages each
time it is asked for, so it cannot drift from them.

- An **alert** entry is one change of alert level at one wadi node. The refresh
  a node sends every minute at Warning and above belongs to the entry it
  repeats. The entry's `id` is the stored id of the alert message that opened
  it; officers acknowledge an alert by that id.
- A **test** entry is a siren node's local test.
- An **all clear** entry ends the alert of one wadi node. The next alert from
  that node opens a new entry, even at the same level.

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


def build_alert_log(messages, site, acknowledgements=None, show_officers=False):
    """The alert log, newest entry first.

    `messages` are the stored messages of `KINDS` in arrival order.
    `acknowledgements` maps an alert entry id to the officer's acknowledgement.
    The officer's name is included only if `show_officers` is set: the log is
    public, the names are for signed-in officers.
    """
    acknowledgements = acknowledgements or {}
    names = {node.id: node.name for node in site.nodes}
    names[site.gateway.id] = site.gateway.name

    entries = []
    # Wadi node id -> its alert entry in force.
    in_force = {}
    # (wadi node id, counter of an alert message) -> the entry that message belongs to.
    by_counter = {}
    # Entry id -> {siren node id: time of its first acknowledgement}.
    answered = {}

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
            entry(message, ALL_CLEAR, wadi_node=wadi_node, wadi_node_name=names.get(wadi_node))
            in_force.pop(wadi_node, None)
        elif kind == "test":
            entry(message, TEST)

    for made in entries:
        if made["type"] != ALERT:
            continue
        made["in_force"] = in_force.get(made["node"]) is made
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
            made["acknowledgement"] = {"ts": acknowledgement.ts}
            if show_officers:
                made["acknowledgement"]["officer"] = acknowledgement.officer
        made["acknowledgement_requested"] = (
            made["in_force"]
            and made["alert_level"] == ACKNOWLEDGEMENT_REQUESTED_AT
            and acknowledgement is None
        )

    entries.reverse()
    return entries
