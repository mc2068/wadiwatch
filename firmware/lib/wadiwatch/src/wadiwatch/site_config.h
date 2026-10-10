// Site configuration: the one place where node identifiers and thresholds are set.
#pragma once

#include <cstdint>

#include "wadiwatch/operating_mode.h"
#include "wadiwatch/packet.h"
#include "wadiwatch/rain_window.h"

namespace wadiwatch {

struct WadiNodeConfig {
  NodeId id;
  // Height of the sensor face above the wadi bed.
  float mountingHeightCm;
  // Water level at or above which the node raises Watch and enters storm mode.
  float watchLevelCm;
  // Water level at or above which the node raises Warning.
  float warningLevelCm;
  OperatingModeConfig mode;
  RainConfig rain;
  uint32_t heartbeatIntervalMs;
};

struct SiteConfig {
  WadiNodeConfig wadiNode;
};

// The bench prototype. Values are starting points, to be calibrated per site.
const SiteConfig& prototypeSite();

}  // namespace wadiwatch
