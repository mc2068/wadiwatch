"""The backend-to-gateway command contract, version 1: what goes down `wadiwatch/down`.

This module is the executable form of the "Commands" section of
docs/gateway-contract.md. There is one command, the all clear for a wadi. The
gateway's reader is firmware/lib/wadiwatch/src/wadiwatch/downlink_command.cpp;
docs/gateway-contract/backend-commands.jsonl holds the shared examples, which
this module must produce byte for byte and that reader must accept.

A command carries an authentication code made with the command key, a secret
the backend shares with the gateway and with nobody else. Anyone who can
publish to the MQTT broker can put a well-formed command on the topic; only
the holder of the key can make one the gateway acts on.
"""

import hashlib
import hmac
import json
import struct

from .contract import CONTRACT_VERSION, MAX_UINT32, ContractError

COMMAND_TOPIC = "wadiwatch/down"
MAX_COMMAND_BYTES = 256
MAX_WADI_ID = 255

ALL_CLEAR = "all_clear"
# In the order the command is written.
FIELDS = ("v", "kind", "command", "ts", "wadi", "auth")

COMMAND_KEY_BYTES = 32
# TEST KEY, NOT SECRET. It is published with the source, so anyone can sign
# commands with it: good for tests and the bench only. The gateway firmware
# has the same one (firmware/lib/wadiwatch/src/wadiwatch/command_key.cpp). A
# real key is never committed: the backend takes it from WADIWATCH_COMMAND_KEY
# (see __main__.py and the README).
TEST_COMMAND_KEY = b"WADIWATCH-TEST-COMMAND-KEY-ONLY!"
assert len(TEST_COMMAND_KEY) == COMMAND_KEY_BYTES

# What the authentication code is computed over: contract version (1 byte), kind
# of command (1 byte), command number (4), ts (4), wadi (1), big-endian.
_SIGNED_FIELDS = struct.Struct(">BBIIB")
_KIND_CODES = {ALL_CLEAR: 1}
# The code as it travels: the whole HMAC-SHA-256 digest in lower-case hexadecimal.
AUTH_CODE_LENGTH = 2 * hashlib.sha256().digest_size
_HEX_DIGITS = frozenset("0123456789abcdef")


class CommandKeyError(ValueError):
    """The command key given to the backend is not a key."""


def command_key_from(setting):
    """The command key for a setting: 32 bytes written as 64 hexadecimal digits.

    Without a setting it is the published test key, which only a bench gateway
    built from the repository as it is will accept.
    """
    if not setting:
        return TEST_COMMAND_KEY
    try:
        key = bytes.fromhex(setting)
    except ValueError:
        raise CommandKeyError("the command key must be written in hexadecimal") from None
    if len(key) != COMMAND_KEY_BYTES:
        raise CommandKeyError(
            f"the command key must be {COMMAND_KEY_BYTES} bytes ({2 * COMMAND_KEY_BYTES} "
            "hexadecimal digits)"
        )
    return key


def command_auth_code(key, command, ts, wadi):
    """The authentication code of an all clear command under `key`.

    HMAC-SHA-256 over the command's fields in a fixed layout, not over its JSON
    text: the gateway reads the fields in any order and with any white space,
    and works out the same code from what it read.
    """
    fields = _SIGNED_FIELDS.pack(CONTRACT_VERSION, _KIND_CODES[ALL_CLEAR], command, ts, wadi)
    return hmac.new(key, fields, hashlib.sha256).hexdigest()


def all_clear_command(command, ts, wadi, key):
    """The all clear command for a wadi, as the bytes to publish.

    `command` is the backend's number for it, `ts` the Unix time it is issued
    at (the gateway ignores a command that is no longer fresh), `wadi` the
    wadi's identifier in the firmware site configuration, `key` the command
    key the gateway holds.
    """
    _check_ranges(command, ts, wadi)
    message = {
        "v": CONTRACT_VERSION,
        "kind": ALL_CLEAR,
        "command": command,
        "ts": ts,
        "wadi": wadi,
        "auth": command_auth_code(key, command, ts, wadi),
    }
    return json.dumps(message, separators=(",", ":"))


def parse_command(payload, key):
    """Read a command back, as strictly as the gateway does, and check that it was
    made by a holder of `key`. Raises ContractError."""
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
    _check_ranges(message["command"], message["ts"], message["wadi"])
    auth = message["auth"]
    if type(auth) is not str or len(auth) != AUTH_CODE_LENGTH or not set(auth) <= _HEX_DIGITS:
        raise ContractError(
            f"field auth must be {AUTH_CODE_LENGTH} hexadecimal digits in lower case"
        )
    expected = command_auth_code(key, message["command"], message["ts"], message["wadi"])
    if not hmac.compare_digest(auth, expected):
        raise ContractError("the authentication code is not the command key's for this command")
    return message


def _check_ranges(command, ts, wadi):
    for name, value, high in (
        ("command", command, MAX_UINT32),
        ("ts", ts, MAX_UINT32),
        ("wadi", wadi, MAX_WADI_ID),
    ):
        if type(value) is not int or not 1 <= value <= high:
            raise ContractError(f"field {name} must be an integer between 1 and {high}")
