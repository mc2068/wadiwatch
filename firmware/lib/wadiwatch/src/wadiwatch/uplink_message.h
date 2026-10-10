// The message the gateway sends up to the backend for one radio packet, as
// docs/gateway-contract.md defines it. This is where packet units and fields are
// translated into those of the contract.
#pragma once

#include <cstddef>
#include <cstdint>

#include "wadiwatch/packet.h"
#include "wadiwatch/site_config.h"

namespace wadiwatch {

// The contract allows no longer message. Give the buffer one byte more, for the
// zero that ends the text.
constexpr size_t kMaxUplinkMessageLength = 512;

// Writes the message for `packet`, received by the gateway at `unixSeconds`, as one
// JSON object with the fields in the order of the contract's tables and no white
// space. Returns its length without the final zero, or 0 if there is no such
// message or it does not fit.
size_t writeUplinkMessage(const Packet& packet,
                          uint32_t unixSeconds,
                          const GatewayConfig& config,
                          char* buffer,
                          size_t capacity);

}  // namespace wadiwatch
