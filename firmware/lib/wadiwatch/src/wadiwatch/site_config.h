// Site configuration: the one place where node identifiers, thresholds and the site key are set.
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

struct SirenNodeConfig {
  NodeId id;
};

struct SiteConfig {
  WadiNodeConfig wadiNode;
  SirenNodeConfig sirenNode;
  // The shared key and the nodes that packets are accepted from.
  SiteAuth auth;
};

// The bench prototype. Values are starting points, to be calibrated per site.
// Its key is a published test key unless a local key file is present (see site_config.cpp).
const SiteConfig& prototypeSite();

}  // namespace wadiwatch
