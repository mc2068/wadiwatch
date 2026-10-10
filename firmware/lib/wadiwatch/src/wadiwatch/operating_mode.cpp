#include "wadiwatch/operating_mode.h"

namespace wadiwatch {

ModeController::ModeController(const OperatingModeConfig& config) : config_(config) {}

void ModeController::onRain(uint64_t nowMs) {
  quietSinceMs_ = nowMs;
  mode_ = OperatingMode::Storm;
}

void ModeController::onLevel(uint64_t nowMs, bool atOrAboveWatchThreshold) {
  if (atOrAboveWatchThreshold) {
    levelAboveWatch_ = true;
    mode_ = OperatingMode::Storm;
    return;
  }
  if (levelAboveWatch_) {
    // First measurement back below the threshold: the quiet period starts here.
    levelAboveWatch_ = false;
    quietSinceMs_ = nowMs;
  }
  if (mode_ == OperatingMode::Storm && nowMs - quietSinceMs_ >= config_.stormQuietMs) {
    mode_ = OperatingMode::Dry;
  }
}

uint32_t ModeController::measureIntervalMs() const {
  return mode_ == OperatingMode::Storm ? config_.stormMeasureIntervalMs
                                       : config_.dryMeasureIntervalMs;
}

}  // namespace wadiwatch
