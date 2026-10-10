// Site configuration: the one place where node identifiers and thresholds are set.
#pragma once

#include <cstdint>

#include "wadiwatch/alert_engine.h"
#include "wadiwatch/operating_mode.h"
#include "wadiwatch/packet.h"
#include "wadiwatch/rain_window.h"

namespace wadiwatch {

struct WadiNodeConfig {
  NodeId id;
  // Height of the sensor face above the wadi bed.
  float mountingHeightCm;
  // Everything the alert engine compares against. The Watch level threshold is
  // also the level at or above which the node is in storm mode.
  AlertThresholds alertThresholds;
  OperatingModeConfig mode;
  RainConfig rain;
  uint32_t heartbeatIntervalMs;
};

struct SirenNodeConfig {
  // Siren pattern 1 (intermittent), sounded at Warning: on for this long, then off for this long.
  uint32_t intermittentOnMs;
  uint32_t intermittentOffMs;
};

struct SiteConfig {
  WadiNodeConfig wadiNode;
  SirenNodeConfig sirenNode;
};

// The bench prototype. Values are starting points, to be calibrated per site.
const SiteConfig& prototypeSite();

}  // namespace wadiwatch
