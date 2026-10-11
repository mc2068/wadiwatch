#include "wadiwatch/site_config.h"

#include <cstring>

// A real site key never goes into the repository. To build firmware for a real site, create
// the file wadiwatch/site_key_local.h next to this one (it is ignored by git) containing
//
//   #define WADIWATCH_SITE_KEY { 0x.., 0x.., ... }   // kSiteKeySize random bytes
//
// and flash every node of the site from the same checkout. The same file holds the
// gateway's command key (command_key.cpp), which is a different key.
#if defined(__has_include)
#if __has_include("wadiwatch/site_key_local.h")
#include "wadiwatch/site_key_local.h"
#endif
#endif

#ifndef WADIWATCH_SITE_KEY
// TEST KEY, NOT SECRET: the ASCII text "WADIWATCH-TEST-KEY-DO-NOT-DEPLOY".
// It is published with the source, so anyone can sign packets with it.
// Good for tests and the bench only.
#define WADIWATCH_SITE_KEY                                                             \
  {                                                                                    \
    'W', 'A', 'D', 'I', 'W', 'A', 'T', 'C', 'H', '-', 'T', 'E', 'S', 'T', '-', 'K',    \
    'E', 'Y', '-', 'D', 'O', '-', 'N', 'O', 'T', '-', 'D', 'E', 'P', 'L', 'O', 'Y'     \
  }
#endif

namespace wadiwatch {

namespace {

constexpr uint32_t kMinuteMs = 60u * 1000u;

const uint8_t kSiteKey[] = WADIWATCH_SITE_KEY;
static_assert(sizeof(kSiteKey) == kSiteKeySize, "the site key must be exactly kSiteKeySize bytes");

const NodeId kWadiNodeId = 0x0101;
const NodeId kDownstreamWadiNodeId = 0x0102;
const NodeId kSirenNodeId = 0x0201;
// The backend knows the gateway by this identifier too (backend/site/prototype.json).
const NodeId kGatewayNodeId = 0x0301;
// The one wadi of the bench prototype.
const WadiId kWadi = 1;

// Every node of the site. Packets from any other sender are ignored.
const NodeId kSiteNodes[] = {kWadiNodeId, kDownstreamWadiNodeId, kSirenNodeId, kGatewayNodeId};
const size_t kSiteNodeCount = sizeof(kSiteNodes) / sizeof(kSiteNodes[0]);
static_assert(kSiteNodeCount <= kMaxSiteNodes, "too many nodes for one site");

SiteConfig makePrototypeSite() {
  SiteConfig site = {
      /* wadiNode */ {
          /* id */ kWadiNodeId,
          /* mountingHeightCm */ 300.0f,
          /* levelSensor */ {
              /* minDistanceCm */ 25.0f,
              // Without a temperature reading, air at -10 C makes the dry bed read
              // 17 cm low over this mounting height.
              /* bedToleranceCm */ 20.0f,
              /* referenceTemperatureC */ 20.0f,
          },
          /* alertThresholds */ {
              /* bankFullDepthCm */ 200.0f,
              /* watchLevelPercent */ 30.0f,
              /* warningLevelPercent */ 50.0f,
              /* evacuateLevelPercent */ 75.0f,
              /* watchRainMm */ 10.0f,
              /* watchRainWindowMs */ 30u * kMinuteMs,
              /* warningRateOfRiseCmPerMin */ 5.0f,
              /* warningRateOfRiseSustainMs */ 3u * kMinuteMs,
          },
          /* mode */ {
              /* dryMeasureIntervalMs */ 15u * kMinuteMs,
              /* stormMeasureIntervalMs */ 1u * kMinuteMs,
              /* stormQuietMs */ 30u * kMinuteMs,
          },
          /* rain */ {
              /* mmPerTip */ 0.2f,
          },
          /* rateOfRise */ {
              /* windowMs */ 5u * kMinuteMs,
          },
          /* heartbeatIntervalMs */ 60u * kMinuteMs,
          /* alertRepeat */ {
              /* transmissions */ 3,
              /* minSpacingMs */ 1000u,
              /* maxSpacingMs */ 4000u,
          },
          /* upstream */ {
              /* node */ kNoNode,
              /* receiveWindowMs */ 3000u,
              // Five alert refresh intervals.
              /* alertRefreshTimeoutMs */ 5u * kMinuteMs,
          },
          /* alertRefreshIntervalMs */ 1u * kMinuteMs,
          /* answerWindowMs */ 2000u,
      },
      /* downstreamWadiNode */ {},  // filled in below
      /* sirenNode */ {
          /* id */ kSirenNodeId,
          /* intermittentOnMs */ 2000u,
          /* intermittentOffMs */ 2000u,
          /* alertRefreshTimeoutMs */ 30u * kMinuteMs,
          /* testDurationMs */ 30u * 1000u,
          /* heartbeatIntervalMs */ 60u * kMinuteMs,
      },
      /* gateway */ {
          /* id */ kGatewayNodeId,
          /* wadis */ {
              {
                  /* id */ kWadi,
                  /* nameArabic */ "وادي تجريبي",
                  /* nameFrench */ "Oued Démo",
                  /* wadiNodes */ {kWadiNodeId, kDownstreamWadiNodeId},
                  /* wadiNodeCount */ 2,
              },
          },
          /* wadiCount */ 1,
          /* smsRetryIntervalMs */ 30u * 1000u,
          // Estimates, to be set from the batteries actually fitted: a single
          // lithium cell in each wadi node, a 12 V battery in the siren node.
          /* batteries */ {
              {/* node */ kWadiNodeId, /* lowBatteryMillivolts */ 3500},
              {/* node */ kDownstreamWadiNodeId, /* lowBatteryMillivolts */ 3500},
              {/* node */ kSirenNodeId, /* lowBatteryMillivolts */ 11800},
          },
          /* batteryCount */ 3,
          /* uplinkRetryIntervalMs */ 30u * 1000u,
          /* allClearRepeat */ {
              /* transmissions */ 3,
              /* minSpacingMs */ 1000u,
              /* maxSpacingMs */ 4000u,
          },
          /* allClearHoldMs */ 90u * 1000u,
          /* allClearAnswerPeriodMs */ 70u * kMinuteMs,
          /* allClearMaxAnswers */ 5,
          /* commandMaxAgeS */ 120u,
      },
      /* auth */ {},
      // A 24-byte packet takes 113 ms on the air with these settings. In storm mode at
      // Warning or above a wadi node sends four a minute (its alert three times and a
      // reading), which is 0.75% of the time. One spreading factor higher (206 ms a
      // packet) would break the duty-cycle limit.
      /* radio */ {
          /* frequencyHz */ 868100000u,
          /* transmitPowerDbm */ 14,
          /* spreadingFactor */ 8,
          /* bandwidthHz */ 125000u,
          /* codingRateDenominator */ 5,
          /* preambleSymbols */ 8,
          /* dutyCycleLimitPercent */ 1.0f,
          /* dutyCyclePeriodMs */ 60u * kMinuteMs,
      },
  };
  // The second wadi node sits downstream of the first on the same wadi. Until each is
  // calibrated at its own cross-section, it starts from the same values.
  site.downstreamWadiNode = site.wadiNode;
  site.downstreamWadiNode.id = kDownstreamWadiNodeId;
  site.downstreamWadiNode.upstream.node = kWadiNodeId;
  std::memcpy(site.auth.key, kSiteKey, kSiteKeySize);
  std::memcpy(site.auth.nodes, kSiteNodes, sizeof(kSiteNodes));
  site.auth.nodeCount = static_cast<uint8_t>(kSiteNodeCount);
  return site;
}

}  // namespace

const SiteConfig& prototypeSite() {
  static const SiteConfig site = makePrototypeSite();
  return site;
}

}  // namespace wadiwatch
