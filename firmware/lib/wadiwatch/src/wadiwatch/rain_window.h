// Rainfall over a sliding window, counted in rain-gauge tips.
#pragma once

#include <cstdint>

namespace wadiwatch {

// The rain gauge fitted to a wadi node.
struct RainConfig {
  // Depth of rain that tips the gauge bucket once.
  float mmPerTip;
};

// Adds up the rain that fell during the window ending now. Pure: the caller
// passes in the time. Fixed memory: the window is kept as a ring of time slots.
class RainWindow {
 public:
  // `windowMs` is the length of the window. The wadi node uses the alert
  // thresholds' Watch rain window, so that `rainMm` is the alert engine's rain input.
  RainWindow(const RainConfig& config, uint32_t windowMs);

  void addTips(uint64_t nowMs, uint32_t tips);

  // Rain during the window ending now. Never misses rain that fell inside the
  // window; may still include rain up to one slot older than the window.
  float rainMm(uint64_t nowMs);

 private:
  // The window is split into this many slots, plus one so that the oldest,
  // partly expired slot still counts.
  enum { kSlotsPerWindow = 30, kSlotCount = kSlotsPerWindow + 1 };

  // Empties the slots that have fallen out of the window by nowMs.
  void rollTo(uint64_t nowMs);

  float mmPerTip_;
  uint32_t slotMs_;
  uint64_t currentSlot_ = 0;
  uint32_t tipsInSlot_[kSlotCount];
};

}  // namespace wadiwatch
