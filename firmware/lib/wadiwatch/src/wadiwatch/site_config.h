// Site configuration: the one place where node identifiers, upstream relationships,
// thresholds, radio settings and the site key are set.
#pragma once

#include <cstdint>

#include "wadiwatch/alert_engine.h"
#include "wadiwatch/operating_mode.h"
#include "wadiwatch/packet.h"
#include "wadiwatch/radio_config.h"
#include "wadiwatch/rain_window.h"
#include "wadiwatch/upstream_link.h"

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
  // Which wadi node is upstream of this one, and how long this one listens for it.
  UpstreamConfig upstream;
  // While at Warning or above, the node sends its alert again this long after it last sent it.
  uint32_t alertResendIntervalMs;
};

struct SirenNodeConfig {
  NodeId id;
  // Siren pattern 1 (intermittent), sounded at Warning: on for this long, then off for this long.
  uint32_t intermittentOnMs;
  uint32_t intermittentOffMs;
};

struct SiteConfig {
  // The site's two wadi nodes. Which one is upstream of the other is decided by
  // `WadiNodeConfig::upstream`, not by these names: in the prototype the first has no
  // upstream node and the second names the first.
  WadiNodeConfig wadiNode;
  WadiNodeConfig downstreamWadiNode;
  SirenNodeConfig sirenNode;
  // The shared key and the nodes that packets are accepted from.
  SiteAuth auth;
  RadioConfig radio;
};

// The bench prototype. Values are starting points, to be calibrated per site.
// Its key is a published test key unless a local key file is present (see site_config.cpp).
const SiteConfig& prototypeSite();

}  // namespace wadiwatch
