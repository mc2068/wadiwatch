// Alert engine: decides the alert level from what a wadi node knows right now.
// Pure: no hardware, no clock, no memory of earlier calls.
#pragma once

#include <cstdint>

#include "wadiwatch/packet.h"

namespace wadiwatch {

// Thresholds of one wadi node. Level thresholds are expressed against its bank-full depth.
struct AlertThresholds {
  // Water level at which the wadi overtops its banks at this node.
  float bankFullDepthCm;
  // Each alert level is raised at or above this percentage of the bank-full depth.
  float watchLevelPercent;
  float warningLevelPercent;
  float evacuateLevelPercent;
  // Rain alone raises Watch: at least this much rain within a window of this length.
  float watchRainMm;
  uint32_t watchRainWindowMs;
  // A fast rise alone raises Warning, however low the level: a rate of rise of
  // at least this much, sustained for this long.
  float warningRateOfRiseCmPerMin;
  uint32_t warningRateOfRiseSustainMs;
};

struct AlertInputs {
  // Filtered water level above the wadi bed.
  float levelCm;
  // The rate of rise that has been sustained for the whole of
  // `AlertThresholds::warningRateOfRiseSustainMs` ending now; zero when the level
  // has not been measured for that long. The caller follows the level over that
  // time; the engine only compares the rate.
  float sustainedRateOfRiseCmPerMin;
  // Rain that fell during the window of `AlertThresholds::watchRainWindowMs` ending now.
  // The caller measures it over that window; the engine only compares the amount.
  float rainInWindowMm;
  // Last alert level heard from the upstream node; None when there is no
  // upstream node or nothing has been heard from it.
  AlertLevel upstreamAlertLevel;
  // The alert level this node has raised and that no all clear has ended yet; None
  // when it has no alert. Upstream confirmation counts a Warning in force here
  // even when the readings no longer call for one.
  AlertLevel alertLevelInForce;
};

// Water level at or above which `alertLevel` is raised by level alone.
float levelThresholdCm(const AlertThresholds& thresholds, AlertLevel alertLevel);

// The alert level these inputs call for. It can be lower than an earlier answer:
// keeping an alert level raised until the all clear is the caller's job.
AlertLevel evaluateAlertLevel(const AlertInputs& inputs, const AlertThresholds& thresholds);

}  // namespace wadiwatch
