#include "wadiwatch/alert_engine.h"

namespace wadiwatch {

namespace {

float percentOfBankFull(const AlertThresholds& thresholds, float percent) {
  return thresholds.bankFullDepthCm * percent / 100.0f;
}

}  // namespace

float levelThresholdCm(const AlertThresholds& thresholds, AlertLevel alertLevel) {
  switch (alertLevel) {
    case AlertLevel::Watch:
      return percentOfBankFull(thresholds, thresholds.watchLevelPercent);
    case AlertLevel::Warning:
      return percentOfBankFull(thresholds, thresholds.warningLevelPercent);
    case AlertLevel::Evacuate:
      return percentOfBankFull(thresholds, thresholds.evacuateLevelPercent);
    case AlertLevel::None:
      break;
  }
  return 0.0f;
}

namespace {

// What the node's own readings call for, whatever its upstream node says.
AlertLevel alertLevelFromReadings(const AlertInputs& inputs, const AlertThresholds& thresholds) {
  if (inputs.levelCm >= levelThresholdCm(thresholds, AlertLevel::Evacuate)) {
    return AlertLevel::Evacuate;
  }
  if (inputs.levelCm >= levelThresholdCm(thresholds, AlertLevel::Warning)) {
    return AlertLevel::Warning;
  }
  // A fast sustained rise is the earliest sign of a flash flood: it calls for
  // Warning however low the level still is, and never for more on its own.
  if (inputs.sustainedRateOfRiseCmPerMin >= thresholds.warningRateOfRiseCmPerMin) {
    return AlertLevel::Warning;
  }
  if (inputs.levelCm >= levelThresholdCm(thresholds, AlertLevel::Watch)) {
    return AlertLevel::Watch;
  }
  // Rain alone never calls for more than Watch.
  if (inputs.rainInWindowMm >= thresholds.watchRainMm) {
    return AlertLevel::Watch;
  }
  return AlertLevel::None;
}

}  // namespace

AlertLevel evaluateAlertLevel(const AlertInputs& inputs, const AlertThresholds& thresholds) {
  const AlertLevel fromReadings = alertLevelFromReadings(inputs, thresholds);
  // Upstream confirmation: Warning or above at this node, called for now or already
  // in force, together with Warning or above at its upstream node calls for Evacuate.
  const AlertLevel here =
      fromReadings > inputs.alertLevelInForce ? fromReadings : inputs.alertLevelInForce;
  if (here >= AlertLevel::Warning && inputs.upstreamAlertLevel >= AlertLevel::Warning) {
    return AlertLevel::Evacuate;
  }
  return fromReadings;
}

}  // namespace wadiwatch
