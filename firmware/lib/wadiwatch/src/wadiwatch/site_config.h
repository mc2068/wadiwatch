// Site configuration: the one place where node identifiers, upstream relationships,
// thresholds, radio settings and the site key are set.
#pragma once

#include <cstddef>
#include <cstdint>

#include "wadiwatch/alert_engine.h"
#include "wadiwatch/alert_repeater.h"
#include "wadiwatch/level_measurement.h"
#include "wadiwatch/operating_mode.h"
#include "wadiwatch/packet.h"
#include "wadiwatch/radio_config.h"
#include "wadiwatch/rain_window.h"
#include "wadiwatch/rate_of_rise.h"
#include "wadiwatch/upstream_link.h"

namespace wadiwatch {

struct WadiNodeConfig {
  NodeId id;
  // Height of the sensor face above the wadi bed.
  float mountingHeightCm;
  // Which readings of the level sensor are physically possible at this node.
  LevelSensorConfig levelSensor;
  // Everything the alert engine compares against. The Watch level threshold is
  // also the level at or above which the node is in storm mode.
  AlertThresholds alertThresholds;
  OperatingModeConfig mode;
  RainConfig rain;
  RateOfRiseConfig rateOfRise;
  uint32_t heartbeatIntervalMs;
  // How each alert is repeated on the air.
  AlertRepeatConfig alertRepeat;
  // Which wadi node is upstream of this one, and how long this one listens for it.
  UpstreamConfig upstream;
  // The alert refresh: while at Warning or above, the node sends its alert again this
  // long after it last sent it. Must be longer than an alert takes to go out all its times.
  uint32_t alertRefreshIntervalMs;
  // The answer window: while the node holds an alert level, or remembers one heard
  // from its upstream node, its receiver stays on for this long after each packet
  // it transmits. That is when the gateway answers with the all clear. Must be
  // longer than the gateway takes to hear a packet and transmit its answer.
  uint32_t answerWindowMs;
};

struct SirenNodeConfig {
  NodeId id;
  // Siren pattern 1 (intermittent), sounded at Warning: on for this long, then off for this long.
  uint32_t intermittentOnMs;
  uint32_t intermittentOffMs;
  // A latched Warning or Evacuate that hears no alert from its wadi node for this
  // long falls silent. Must be several times the wadi node's alert refresh interval.
  uint32_t alertRefreshTimeoutMs;
  // A test started with the button ends by itself after this long.
  uint32_t testDurationMs;
  uint32_t heartbeatIntervalMs;
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
  // How each all clear packet is repeated on the air, as a wadi node repeats an alert.
  AlertRepeatConfig allClearRepeat;
  // A wadi node hears the all clear only in the answer window after a packet of its
  // own. The gateway holds the all clear for a wadi node until it next hears that
  // node, and for this long at most: then it transmits anyway, for the siren nodes.
  // Must be longer than the wadi nodes' alert refresh interval, so that a node at
  // Warning or above is heard first.
  uint32_t allClearHoldMs;
  // A wadi node that held an alert level when the all clear was issued is given the
  // all clear again whenever it shows it has not heard, for this long after the
  // command. After that its alert counts as a new event. Must be longer than the
  // heartbeat interval: a node at Watch in dry mode sends nothing else.
  uint32_t allClearAnswerPeriodMs;
  // And this many times at most for one command.
  uint8_t allClearMaxAnswers;
  // A command from the backend is acted on only if it was issued no longer ago than
  // this by the gateway's wall clock, and not in the future by more: a command the
  // broker kept through an outage must not end a later event.
  uint32_t commandMaxAgeS;
};

struct SiteConfig {
  // The site's two wadi nodes. Which one is upstream of the other is decided by
  // `WadiNodeConfig::upstream`, not by these names: in the prototype the first has no
  // upstream node and the second names the first.
  WadiNodeConfig wadiNode;
  WadiNodeConfig downstreamWadiNode;
  SirenNodeConfig sirenNode;
  GatewayConfig gateway;
  // The shared key and the nodes that packets are accepted from.
  SiteAuth auth;
  RadioConfig radio;
};

// The bench prototype. Values are starting points, to be calibrated per site.
// Its key is a published test key unless a local key file is present (see site_config.cpp).
const SiteConfig& prototypeSite();

}  // namespace wadiwatch
