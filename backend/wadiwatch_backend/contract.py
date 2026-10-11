"""The gateway-to-backend message contract, version 1.

This module is the executable form of docs/gateway-contract.md. Change the two
together, and keep docs/gateway-contract/*.jsonl passing.
"""

import json
import math
from dataclasses import dataclass, field

CONTRACT_VERSION = 1
MAX_MESSAGE_BYTES = 512
MAX_UINT16 = 0xFFFF
MAX_UINT32 = 0xFFFFFFFF

# In order of severity.
ALERT_LEVELS = ("none", "watch", "warning", "evacuate")

# Sanity bounds: anything outside is a corrupt message, not a measurement.
MAX_LEVEL_CM = 10_000
MAX_RISE_CM_MIN = 1_000
MAX_RAIN_MM = 10_000
MIN_RSSI_DBM = -200

WADI_NODE = "wadi_node"
SIREN_NODE = "siren_node"
GATEWAY = "gateway"


class ContractError(ValueError):
    """The payload is not a valid gateway message."""


def _integer(low, high):
    def check(value):
        if type(value) is not int:
            return "must be an integer"
        if not low <= value <= high:
            return f"must be between {low} and {high}"
        return None

    return check


def _number(low, high):
    def check(value):
        if type(value) not in (int, float):
            return "must be a number"
        if not math.isfinite(value) or not low <= value <= high:
            return f"must be between {low} and {high}"
        return None

    return check


def _boolean(value):
    return None if type(value) is bool else "must be true or false"


def _one_of(choices):
    def check(value):
        if type(value) is not str or value not in choices:
            return "must be one of " + ", ".join(choices)
        return None

    return check


_node_id = _integer(1, MAX_UINT16)
_level_cm = _number(0, MAX_LEVEL_CM)
_rise_cm_min = _number(-MAX_RISE_CM_MIN, MAX_RISE_CM_MIN)

COMMON_FIELDS = {
    "node": _node_id,
    "counter": _integer(0, MAX_UINT32),
    "ts": _integer(1, MAX_UINT32),
}


@dataclass(frozen=True)
class Kind:
    # Roles allowed to send this kind of message.
    senders: tuple
    required: dict = field(default_factory=dict)
    optional: dict = field(default_factory=dict)


KINDS = {
    "reading": Kind(
        senders=(WADI_NODE,),
        required={
            "level_cm": _level_cm,
            "rise_cm_min": _rise_cm_min,
        },
        # The gateway sends all four with every reading. They are optional so
        # that messages written before they existed stay valid.
        optional={
            "rain_mm": _number(0, MAX_RAIN_MM),
            "storm_mode": _boolean,
            # What the node held when it measured. Only an alert message
            # changes the alert level the backend shows.
            "alert_level": _one_of(ALERT_LEVELS),
            # The node could not measure: level_cm repeats an earlier level.
            "level_repeated": _boolean,
        },
    ),
    "alert": Kind(
        senders=(WADI_NODE,),
        required={"alert_level": _one_of(ALERT_LEVELS)},
        # The measurement behind the alert, whether the alert repeats a level
        # the node already announced, and whether level_cm repeats an earlier
        # level because the node could not measure. The gateway sends all four.
        optional={
            "level_cm": _level_cm,
            "rise_cm_min": _rise_cm_min,
            "refresh": _boolean,
            "level_repeated": _boolean,
        },
    ),
    "acknowledgement": Kind(
        senders=(SIREN_NODE,),
        required={
            "wadi_node": _node_id,
            "alert_level": _one_of(ALERT_LEVELS[1:]),
        },
        optional={"alert_counter": _integer(0, MAX_UINT32)},
    ),
    "heartbeat": Kind(
        senders=(WADI_NODE, SIREN_NODE),
        required={
            "battery_mv": _integer(0, MAX_UINT16),
            "rssi_dbm": _integer(MIN_RSSI_DBM, 0),
            "low_battery": _boolean,
            "tamper": _boolean,
        },
        optional={
            "level_cm": _level_cm,
            "level_repeated": _boolean,
            "discards": _integer(0, MAX_UINT32),
            "refresh_lost": _boolean,
        },
    ),
    "all_clear": Kind(
        senders=(GATEWAY,),
        required={"wadi_node": _node_id},
        # The backend's number for the command the gateway relayed (downlink.py).
        # The gateway always sends it; a message written by hand may leave it out.
        optional={"command": _integer(1, MAX_UINT32)},
    ),
    "test": Kind(senders=(SIREN_NODE,)),
}


def _reject_constant(name):
    raise ContractError(f"{name} is not a JSON number")


def _unique_fields(pairs):
    fields = {}
    for name, value in pairs:
        if name in fields:
            raise ContractError(f"field {name} is given twice")
        fields[name] = value
    return fields


def parse_message(payload, site):
    """Return the message as a dict, or raise ContractError saying what is wrong.

    `payload` is the raw MQTT or HTTP body (bytes or str). `site` supplies the
    configured nodes: a message from or about a node outside it is rejected.
    """
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    if len(payload) > MAX_MESSAGE_BYTES:
        raise ContractError(f"message is longer than {MAX_MESSAGE_BYTES} bytes")
    try:
        message = json.loads(
            payload.decode("utf-8"),
            parse_constant=_reject_constant,
            object_pairs_hook=_unique_fields,
        )
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError) as error:
        raise ContractError(f"not JSON: {error}") from None

    if type(message) is not dict:
        raise ContractError("message must be a JSON object")
    if "v" not in message:
        raise ContractError("field v is missing")
    if type(message["v"]) is not int or message["v"] != CONTRACT_VERSION:
        raise ContractError(f"field v must be {CONTRACT_VERSION}")
    kind = KINDS.get(message.get("kind")) if type(message.get("kind")) is str else None
    if kind is None:
        raise ContractError("field kind must be one of " + ", ".join(KINDS))

    required = {**COMMON_FIELDS, **kind.required}
    allowed = {"v", "kind", *required, *kind.optional}
    for name in required:
        if name not in message:
            raise ContractError(f"field {name} is missing")
    for name in message:
        if name not in allowed:
            raise ContractError(f"field {name} is not part of a {message['kind']} message")
    for name, check in {**required, **kind.optional}.items():
        if name in message:
            problem = check(message[name])
            if problem:
                raise ContractError(f"field {name} {problem}")

    sender_role = site.role_of(message["node"])
    if sender_role is None:
        raise ContractError(f"node {message['node']} is not in the site file")
    if sender_role not in kind.senders:
        raise ContractError(f"a {sender_role} does not send {message['kind']} messages")
    if "wadi_node" in message and site.role_of(message["wadi_node"]) != WADI_NODE:
        raise ContractError(f"wadi_node {message['wadi_node']} is not a wadi node of the site")

    return message
