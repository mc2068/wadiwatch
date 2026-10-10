// Site configuration: the one place where node identifiers, thresholds and the site key are set.
#pragma once

#include <cstddef>
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
  NodeId id;
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

// The battery voltage below which the gateway reports a node as low on battery.
// Heartbeat packets carry the voltage, not a verdict, and a wadi node and a siren
// node do not run on the same battery.
struct BatteryConfig {
  NodeId node;
  uint16_t lowBatteryMillivolts;
};

// The recipient list is not part of this: it stays on the gateway (recipients.h).
struct GatewayConfig {
  // The gateway is a node of the site: it needs an identifier to keep its own
  // packet counters, and to sign what it sends.
  NodeId id;
  WadiConfig wadis[kMaxWadis];
  uint8_t wadiCount;
  // After the GSM modem fails to send an SMS, the gateway waits this long
  // before it asks the modem again.
  uint32_t smsRetryIntervalMs;
  // One entry per node that sends heartbeats. A node not listed here is never
  // reported as low on battery.
  BatteryConfig batteries[kMaxSiteNodes];
  uint8_t batteryCount;
  // After the uplink fails to take a message, the gateway waits this long before
  // it offers the message again.
  uint32_t uplinkRetryIntervalMs;
};

struct SiteConfig {
  WadiNodeConfig wadiNode;
  SirenNodeConfig sirenNode;
  GatewayConfig gateway;
  // The shared key and the nodes that packets are accepted from.
  SiteAuth auth;
};

// The bench prototype. Values are starting points, to be calibrated per site.
// Its key is a published test key unless a local key file is present (see site_config.cpp).
const SiteConfig& prototypeSite();

}  // namespace wadiwatch
