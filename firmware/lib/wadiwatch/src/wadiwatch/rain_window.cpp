#include "wadiwatch/rain_window.h"

namespace wadiwatch {

RainWindow::RainWindow(const RainConfig& config, uint32_t windowMs)
    : mmPerTip_(config.mmPerTip), slotMs_(windowMs / kSlotsPerWindow) {
  if (slotMs_ == 0) slotMs_ = 1;
  for (uint32_t& tips : tipsInSlot_) tips = 0;
}

void RainWindow::addTips(uint64_t nowMs, uint32_t tips) {
  rollTo(nowMs);
  tipsInSlot_[currentSlot_ % kSlotCount] += tips;
}

float RainWindow::rainMm(uint64_t nowMs) {
  rollTo(nowMs);
  uint32_t tips = 0;
  for (uint32_t tipsInSlot : tipsInSlot_) tips += tipsInSlot;
  return static_cast<float>(tips) * mmPerTip_;
}

void RainWindow::rollTo(uint64_t nowMs) {
  const uint64_t slot = nowMs / slotMs_;
  if (slot <= currentSlot_) return;
  uint64_t slotsToEmpty = slot - currentSlot_;
  if (slotsToEmpty > kSlotCount) slotsToEmpty = kSlotCount;
  for (uint64_t i = 0; i < slotsToEmpty; ++i) {
    tipsInSlot_[(slot - i) % kSlotCount] = 0;
  }
  currentSlot_ = slot;
}

}  // namespace wadiwatch
