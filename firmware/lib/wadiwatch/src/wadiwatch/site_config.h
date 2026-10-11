// Site configuration: the one place where node identifiers and thresholds are set.
#pragma once

#include <cstdint>

#include "wadiwatch/packet.h"

namespace wadiwatch {

struct WadiNodeConfig {
  NodeId id;
  // Height of the sensor face above the wadi bed.
  float mountingHeightCm;
  // Water level at or above which the node raises Warning.
  float warningLevelCm;
  uint32_t measureIntervalMs;
};

struct SiteConfig {
  WadiNodeConfig wadiNode;
};

// The bench prototype. Values are starting points, to be calibrated per site.
const SiteConfig& prototypeSite();

}  // namespace wadiwatch
