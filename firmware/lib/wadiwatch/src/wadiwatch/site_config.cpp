#include "wadiwatch/site_config.h"

namespace wadiwatch {

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
          },
          /* measureIntervalMs */ 60u * 1000u,
      },
      /* sirenNode */ {
          /* intermittentOnMs */ 2000u,
          /* intermittentOffMs */ 2000u,
      },
  };
  return site;
}

}  // namespace wadiwatch
