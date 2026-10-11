"""A stand-in for the gateway, for trying the dashboard without hardware.

The real all clear goes down to the gateway over MQTT, is transmitted on the
radio and comes back as `all_clear` messages. With no gateway (a backend fed
by the replay tool) the officer's control would send its command nowhere. This
stand-in takes the command and answers as the gateway would: one `all_clear`
message for each wadi node of the wadi, through the same ingest as everything
else.

Like the gateway it acts only on a command that carries the authentication
code of the command key it holds.

It sounds no siren and sends no SMS, and nothing in it is the gateway's code.
It is only switched on by WADIWATCH_GATEWAY=simulated (see __main__.py): never
use it where a real gateway is listening.
"""

import json
import logging

from .contract import CONTRACT_VERSION, ContractError
from .downlink import parse_command

log = logging.getLogger("wadiwatch.simulated_gateway")


class SimulatedGateway:
    def __init__(self, backend, clock, command_key):
        self._backend = backend
        self._clock = clock
        self._command_key = command_key
        self._counter = None

    def __call__(self, payload):
        """Take one command, as the downlink of a Backend. Returns True if it was acted on."""
        try:
            command = parse_command(payload, self._command_key)
        except ContractError as error:
            log.warning("simulated gateway: command refused: %s", error)
            return False
        site = self._backend.site
        for node in site.wadi_nodes_of(command["wadi"]):
            self._backend.ingest(
                json.dumps(
                    {
                        "v": CONTRACT_VERSION,
                        "kind": "all_clear",
                        "node": site.gateway.id,
                        "counter": self._next_counter(),
                        "ts": int(self._clock()),
                        "wadi_node": node.id,
                        "command": command["command"],
                    }
                )
            )
        log.info("simulated gateway: all clear for wadi %d", command["wadi"])
        return True

    def _next_counter(self):
        # Carries on from whatever the backend already holds from the gateway, so
        # that an all clear after a replayed scenario is not taken for a duplicate.
        if self._counter is None:
            self._counter = self._backend.latest_counter(self._backend.site.gateway.id)
        self._counter += 1
        return self._counter
