// Site configuration: the one place where node identifiers and thresholds are set.
#pragma once

#include <cstddef>
#include <cstdint>

#include "wadiwatch/alert_engine.h"
#include "wadiwatch/packet.h"

namespace wadiwatch {

struct WadiNodeConfig {
  NodeId id;
  // Height of the sensor face above the wadi bed.
  float mountingHeightCm;
  AlertThresholds alertThresholds;
  uint32_t measureIntervalMs;
};

struct SirenNodeConfig {
  // Siren pattern 1 (intermittent), sounded at Warning: on for this long, then off for this long.
  uint32_t intermittentOnMs;
  uint32_t intermittentOffMs;
};

// Identifies a wadi within the site. Wadi nodes are mounted over a wadi,
// recipients register near one, and SMS name it.
using WadiId = uint8_t;

constexpr size_t kMaxWadis = 4;
constexpr size_t kMaxWadiNodesPerWadi = 4;

struct WadiConfig {
  WadiId id;
  // The wadi's name as SMS give it, in each language (UTF-8).
  const char* nameArabic;
  const char* nameFrench;
  // The wadi nodes mounted over this wadi. The gateway acts on alerts from
  // these wadi nodes only.
  NodeId wadiNodes[kMaxWadiNodesPerWadi];
  uint8_t wadiNodeCount;
};

// The recipient list is not part of this: it stays on the gateway (recipients.h).
struct GatewayConfig {
  WadiConfig wadis[kMaxWadis];
  uint8_t wadiCount;
  // After the GSM modem fails to send an SMS, the gateway waits this long
  // before it asks the modem again.
  uint32_t smsRetryIntervalMs;
};

struct SiteConfig {
  WadiNodeConfig wadiNode;
  SirenNodeConfig sirenNode;
  GatewayConfig gateway;
};

// The bench prototype. Values are starting points, to be calibrated per site.
const SiteConfig& prototypeSite();

}  // namespace wadiwatch
