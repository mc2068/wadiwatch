// Alert engine: decides the alert level from what a wadi node knows right now.
// Pure: no hardware, no clock, no memory of earlier calls.
#pragma once

#include "wadiwatch/packet.h"

namespace wadiwatch {

// Level thresholds of one wadi node, expressed against its bank-full depth.
struct AlertThresholds {
  // Water level at which the wadi overtops its banks at this node.
  float bankFullDepthCm;
  // Each alert level is raised at or above this percentage of the bank-full depth.
  float watchLevelPercent;
  float warningLevelPercent;
  float evacuateLevelPercent;
};

struct AlertInputs {
  // Filtered water level above the wadi bed.
  float levelCm;
  float rateOfRiseCmPerMin;
  float rainfallIntensityMmPerHour;
  // Last alert level heard from the upstream node; None when there is no
  // upstream node or nothing has been heard from it.
  AlertLevel upstreamAlertLevel;
};

// Water level at or above which `alertLevel` is raised by level alone.
float levelThresholdCm(const AlertThresholds& thresholds, AlertLevel alertLevel);

// The alert level these inputs call for. It can be lower than an earlier answer:
// keeping an alert level raised until the all clear is the caller's job.
AlertLevel evaluateAlertLevel(const AlertInputs& inputs, const AlertThresholds& thresholds);

}  // namespace wadiwatch
