"""The backend-to-gateway command contract, version 1: what goes down `wadiwatch/down`.

This module is the executable form of the "Commands" section of
docs/gateway-contract.md. There is one command, the all clear for a wadi. The
gateway's reader is firmware/lib/wadiwatch/src/wadiwatch/downlink_command.cpp;
docs/gateway-contract/backend-commands.jsonl holds the shared examples, which
this module must produce byte for byte and that reader must accept.
"""

import json

from .contract import CONTRACT_VERSION, MAX_UINT32, ContractError

COMMAND_TOPIC = "wadiwatch/down"
MAX_COMMAND_BYTES = 256
MAX_WADI_ID = 255

ALL_CLEAR = "all_clear"
# In the order the command is written.
FIELDS = ("v", "kind", "command", "ts", "wadi")


def all_clear_command(command, ts, wadi):
    """The all clear command for a wadi, as the bytes to publish.

    `command` is the backend's number for it, `ts` the Unix time it is issued
    at (the gateway ignores a command that is no longer fresh), `wadi` the
    wadi's identifier in the firmware site configuration.
    """
    for name, value, high in (("command", command, MAX_UINT32), ("ts", ts, MAX_UINT32), ("wadi", wadi, MAX_WADI_ID)):
        if type(value) is not int or not 1 <= value <= high:
            raise ContractError(f"field {name} must be an integer between 1 and {high}")
    message = {"v": CONTRACT_VERSION, "kind": ALL_CLEAR, "command": command, "ts": ts, "wadi": wadi}
    return json.dumps(message, separators=(",", ":"))


def parse_command(payload):
    """Read a command back, as strictly as the gateway does. Raises ContractError."""
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    if len(payload) > MAX_COMMAND_BYTES:
        raise ContractError(f"command is longer than {MAX_COMMAND_BYTES} bytes")
    try:
        message = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError) as error:
        raise ContractError(f"not JSON: {error}") from None
    if type(message) is not dict or set(message) != set(FIELDS):
        raise ContractError("a command has exactly the fields " + ", ".join(FIELDS))
    if type(message["v"]) is not int or message["v"] != CONTRACT_VERSION:
        raise ContractError(f"field v must be {CONTRACT_VERSION}")
    if message["kind"] != ALL_CLEAR:
        raise ContractError(f"field kind must be {ALL_CLEAR}")
    # Building it again applies the range checks.
    all_clear_command(message["command"], message["ts"], message["wadi"])
    return message
