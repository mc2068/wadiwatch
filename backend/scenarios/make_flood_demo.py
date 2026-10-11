"""Write flood_demo.jsonl: the gateway messages of one flood on the prototype site.

    python scenarios/make_flood_demo.py        (from backend/)

The scenario is synthetic. It stands in for a recording until the hardware
exists, and follows the behaviour the specification describes: storm mode at
the first rain, Watch, Warning and Evacuate against a 200 cm bank-full depth,
Evacuate at the downstream node by upstream confirmation, alert refreshes and
siren acknowledgements every minute, and an all clear thirty minutes after
both levels are back below the Watch threshold. Run it again after changing
the contract or the site file, and replace it with a real recording when one
exists.
"""

import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
SITE_FILE = HERE.parent / "site" / "prototype.json"
OUTPUT = HERE / "flood_demo.jsonl"

CONTRACT_VERSION = 1
# 2026-10-10 14:00:00 UTC. The replay tool moves the times to the present.
START_TS = 1_791_640_800

BANK_FULL_CM = 200.0
WATCH_CM = 0.30 * BANK_FULL_CM
WARNING_CM = 0.50 * BANK_FULL_CM

RAIN_STARTS_MIN = 20
ALL_CLEAR_MIN = 101
END_MIN = 105
DRY_INTERVAL_MIN = 15
HEARTBEAT_INTERVAL_MIN = 60
RISE_WINDOW_MIN = 5
SIREN_TEST_MIN = 5

# Water level as (minute, cm) points joined by straight lines.
UPSTREAM_LEVEL = [
    (0, 8), (20, 8), (24, 20), (29, 62), (36, 104), (44, 128), (50, 110),
    (56, 80), (62, 58), (72, 30), (85, 15), (105, 10),
]
DOWNSTREAM_LEVEL = [
    (0, 6), (28, 6), (32, 18), (37, 61), (44, 102), (52, 136), (58, 112),
    (64, 82), (70, 57), (80, 30), (92, 14), (105, 9),
]


def level_at(curve, minute):
    minute = max(minute, curve[0][0])
    for (m0, l0), (m1, l1) in zip(curve, curve[1:]):
        if m0 <= minute <= m1:
            return l0 + (l1 - l0) * (minute - m0) / (m1 - m0)
    return curve[-1][1]


def main():
    site = json.loads(SITE_FILE.read_text(encoding="utf-8"))
    wadi_nodes = [node for node in site["nodes"] if node["role"] == "wadi_node"]
    upstream = next(node["id"] for node in wadi_nodes if node["upstream"] is None)
    downstream = next(node["id"] for node in wadi_nodes if node["upstream"] == upstream)
    siren = next(node["id"] for node in site["nodes"] if node["role"] == "siren_node")
    gateway = site["gateway"]["id"]

    curves = {upstream: UPSTREAM_LEVEL, downstream: DOWNSTREAM_LEVEL}
    # Seconds after the minute at which each wadi node transmits.
    slot = {upstream: 0, downstream: 5}
    counters = {upstream: 1200, downstream: 870, siren: 310, gateway: 12}
    alert_level = {upstream: "none", downstream: "none"}
    messages = []

    def emit(second, kind, node, **fields):
        counters[node] += 1
        messages.append(
            {
                "v": CONTRACT_VERSION,
                "kind": kind,
                "node": node,
                "counter": counters[node],
                "ts": START_TS + second,
                **fields,
            }
        )
        return counters[node]

    def decide(node, level_cm):
        """The alert level the node would raise. Levels only escalate."""
        current = alert_level[node]
        if current == "none" and level_cm > WATCH_CM:
            return "watch"
        if current == "watch" and level_cm > WARNING_CM:
            return "warning"
        if current == "warning" and node == downstream and alert_level[upstream] == "warning":
            return "evacuate"
        return current

    for minute in range(END_MIN + 1):
        storm = RAIN_STARTS_MIN <= minute <= ALL_CLEAR_MIN
        for node in (upstream, downstream):
            second = minute * 60 + slot[node]
            if storm or minute % DRY_INTERVAL_MIN == 0:
                level_cm = round(level_at(curves[node], minute), 1)
                rise = (level_cm - level_at(curves[node], minute - RISE_WINDOW_MIN)) / RISE_WINDOW_MIN
                emit(second, "reading", node, level_cm=level_cm, rise_cm_min=round(rise, 1))
                if minute < ALL_CLEAR_MIN:
                    raised = decide(node, level_cm)
                    changed = raised != alert_level[node]
                    alert_level[node] = raised
                    # A change is sent at once; Warning and above are refreshed every minute.
                    if changed or raised in ("warning", "evacuate"):
                        alert_counter = emit(second + 1, "alert", node, alert_level=raised)
                        if raised in ("warning", "evacuate"):
                            emit(
                                second + 3,
                                "acknowledgement",
                                siren,
                                wadi_node=node,
                                alert_level=raised,
                                alert_counter=alert_counter,
                            )
            if minute % HEARTBEAT_INTERVAL_MIN == 0:
                emit(
                    second + 20,
                    "heartbeat",
                    node,
                    battery_mv=3960 - minute,
                    rssi_dbm=-94 if node == upstream else -81,
                    low_battery=False,
                    tamper=False,
                    level_cm=round(level_at(curves[node], minute), 1),
                    discards=0,
                )
        if minute % HEARTBEAT_INTERVAL_MIN == 0:
            emit(
                minute * 60 + 30,
                "heartbeat",
                siren,
                battery_mv=12600 - 2 * minute,
                rssi_dbm=-76,
                low_battery=False,
                tamper=False,
                refresh_lost=False,
            )
        if minute == SIREN_TEST_MIN:
            emit(minute * 60 + 40, "test", siren)
        if minute == ALL_CLEAR_MIN:
            emit(minute * 60 + 40, "all_clear", gateway, wadi_node=upstream)
            emit(minute * 60 + 43, "all_clear", gateway, wadi_node=downstream)

    messages.sort(key=lambda message: message["ts"])
    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as output:
        for message in messages:
            output.write(json.dumps(message, separators=(",", ":")) + "\n")
    print(f"{len(messages)} messages written to {OUTPUT}")


if __name__ == "__main__":
    main()
