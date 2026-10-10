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
          /* watchLevelCm */ 60.0f,
          /* warningLevelCm */ 100.0f,
          /* mode */ {
              /* dryMeasureIntervalMs */ 15u * kMinuteMs,
              /* stormMeasureIntervalMs */ 1u * kMinuteMs,
              /* stormQuietMs */ 30u * kMinuteMs,
          },
          /* rain */ {
              /* mmPerTip */ 0.2f,
              /* watchRainMm */ 10.0f,
              /* watchRainWindowMs */ 30u * kMinuteMs,
          },
          /* heartbeatIntervalMs */ 60u * kMinuteMs,
      },
  };
  return site;
}

}  // namespace wadiwatch
