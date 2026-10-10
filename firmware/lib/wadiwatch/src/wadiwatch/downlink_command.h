// The command the backend sends down to the gateway, as docs/gateway-contract.md
// defines it under "Commands". There is one: the all clear for a wadi.
#pragma once

#include <cstddef>
#include <cstdint>

#include "wadiwatch/site_config.h"

namespace wadiwatch {

// The contract allows no longer command. Give a receive buffer one byte more.
constexpr size_t kMaxDownlinkCommandLength = 256;

// An officer's all clear for one wadi, as the backend issued it.
struct AllClearCommand {
  // The backend's number for the command, never zero.
  uint32_t command;
  // When the backend issued it: Unix seconds, UTC.
  uint32_t issuedAt;
  WadiId wadi;
};

// Reads one command: a flat JSON object with exactly the fields of the contract,
// each a whole number in its range or one of the contract's words. Returns false,
// leaving `command` alone, for anything else: text that is not such an object, a
// missing or repeated field, a field the contract does not list, another version
// or kind. A command that is refused is never acted on in part.
bool parseAllClearCommand(const char* text, size_t length, AllClearCommand& command);

}  // namespace wadiwatch
