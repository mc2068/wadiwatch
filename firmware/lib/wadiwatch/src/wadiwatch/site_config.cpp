#include "wadiwatch/site_config.h"

namespace wadiwatch {

namespace {

constexpr uint32_t kMinuteMs = 60u * 1000u;

}  // namespace

const SiteConfig& prototypeSite() {
  static const SiteConfig site = {
      /* wadiNode */ {
          /* id */ 0x0101,
          /* mountingHeightCm */ 300.0f,
          /* alertThresholds */ {
              /* bankFullDepthCm */ 200.0f,
              /* watchLevelPercent */ 30.0f,
              /* warningLevelPercent */ 50.0f,
              /* evacuateLevelPercent */ 75.0f,
              /* watchRainMm */ 10.0f,
              /* watchRainWindowMs */ 30u * kMinuteMs,
          },
          /* mode */ {
              /* dryMeasureIntervalMs */ 15u * kMinuteMs,
              /* stormMeasureIntervalMs */ 1u * kMinuteMs,
              /* stormQuietMs */ 30u * kMinuteMs,
          },
          /* rain */ {
              /* mmPerTip */ 0.2f,
          },
          /* heartbeatIntervalMs */ 60u * kMinuteMs,
      },
      /* sirenNode */ {
          /* intermittentOnMs */ 2000u,
          /* intermittentOffMs */ 2000u,
      },
  };
  return site;
}

}  // namespace wadiwatch
