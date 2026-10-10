#include "wadiwatch/site_config.h"

namespace wadiwatch {

const SiteConfig& prototypeSite() {
  static const SiteConfig site = {
      /* wadiNode */ {
          /* id */ 0x0101,
          /* mountingHeightCm */ 300.0f,
          /* warningLevelCm */ 100.0f,
          /* measureIntervalMs */ 60u * 1000u,
      },
  };
  return site;
}

}  // namespace wadiwatch
