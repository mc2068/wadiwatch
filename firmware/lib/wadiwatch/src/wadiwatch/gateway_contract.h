// What both directions of docs/gateway-contract.md share on the gateway: the messages
// it sends up (uplink_message.h) and the command it reads (downlink_command.h).
#pragma once

#include <cstdint>

namespace wadiwatch {

// The `v` of every message and every command.
constexpr uint32_t kContractVersion = 1;

}  // namespace wadiwatch
