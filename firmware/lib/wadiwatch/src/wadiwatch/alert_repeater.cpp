#include "wadiwatch/alert_repeater.h"

#include <cstring>

namespace wadiwatch {

AlertRepeater::AlertRepeater(const AlertRepeatConfig& config, RandomSource& random)
    : config_(config), random_(random) {}

void AlertRepeater::start(const uint8_t* frame, size_t length, uint64_t nowMs) {
  remaining_ = 0;
  if (length == 0 || length > sizeof(frame_) || config_.transmissions < 2) return;
  std::memcpy(frame_, frame, length);
  length_ = length;
  remaining_ = static_cast<uint8_t>(config_.transmissions - 1);
  dueAtMs_ = nowMs + drawSpacingMs();
}

const uint8_t* AlertRepeater::takeDue(uint64_t nowMs, size_t& length) {
  if (remaining_ == 0 || nowMs < dueAtMs_) return nullptr;
  --remaining_;
  // Spacing is counted from the moment of transmission, so a node that woke
  // late never sends two copies back to back.
  if (remaining_ > 0) dueAtMs_ = nowMs + drawSpacingMs();
  length = length_;
  return frame_;
}

uint32_t AlertRepeater::drawSpacingMs() {
  if (config_.maxSpacingMs <= config_.minSpacingMs) return config_.minSpacingMs;
  const uint32_t span = config_.maxSpacingMs - config_.minSpacingMs;
  // Both ends can be drawn. A span that covers every 32-bit value has no room for the "+ 1".
  const uint32_t bound = span == UINT32_MAX ? span : span + 1;
  // The remainder keeps a misbehaving source from stretching the spacing past its longest.
  return config_.minSpacingMs + random_.below(bound) % bound;
}

}  // namespace wadiwatch
