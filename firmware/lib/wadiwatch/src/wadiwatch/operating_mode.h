// Dry mode and storm mode: how often the wadi node measures.
#pragma once

#include <cstdint>

namespace wadiwatch {

enum class OperatingMode : uint8_t { Dry, Storm };

struct OperatingModeConfig {
  uint32_t dryMeasureIntervalMs;
  uint32_t stormMeasureIntervalMs;
  // The node returns to dry mode once the level has stayed below the Watch
  // threshold, with no rain, for this long.
  uint32_t stormQuietMs;
};

// Decides the operating mode from what the node observes. Pure: the caller
// passes in the time and the observations.
class ModeController {
 public:
  explicit ModeController(const OperatingModeConfig& config);

  // The rain gauge has tipped since the last call.
  void onRain(uint64_t nowMs);
  // A level measurement was taken.
  void onLevel(uint64_t nowMs, bool atOrAboveWatchThreshold);

  OperatingMode mode() const { return mode_; }
  // Time from one measurement to the next in the current mode.
  uint32_t measureIntervalMs() const;

 private:
  OperatingModeConfig config_;
  OperatingMode mode_ = OperatingMode::Dry;
  bool levelAboveWatch_ = false;
  // Since when there has been no rain and no level at or above the Watch threshold.
  uint64_t quietSinceMs_ = 0;
};

}  // namespace wadiwatch
