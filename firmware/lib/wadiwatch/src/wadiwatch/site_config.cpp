#include "wadiwatch/site_config.h"

namespace wadiwatch {

namespace {

constexpr NodeId kWadiNodeId = 0x0101;
// The one wadi of the bench prototype.
constexpr WadiId kWadi = 1;

}  // namespace

const SiteConfig& prototypeSite() {
  static const SiteConfig site = {
      /* wadiNode */ {
          /* id */ kWadiNodeId,
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
      /* gateway */ {
          /* wadis */ {
              {
                  /* id */ kWadi,
                  /* nameArabic */ "وادي تجريبي",
                  /* nameFrench */ "Oued Démo",
                  /* wadiNodes */ {kWadiNodeId},
                  /* wadiNodeCount */ 1,
              },
          },
          /* wadiCount */ 1,
          /* smsRetryIntervalMs */ 30u * 1000u,
      },
  };
  return site;
}

}  // namespace wadiwatch
