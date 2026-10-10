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

AlertLevel evaluateAlertLevel(const AlertInputs& inputs, const AlertThresholds& thresholds) {
  // Only the level acts so far. Rate of rise, rainfall intensity and the
  // upstream node are accepted here so that callers already pass them.
  if (inputs.levelCm >= levelThresholdCm(thresholds, AlertLevel::Evacuate)) {
    return AlertLevel::Evacuate;
  }
  if (inputs.levelCm >= levelThresholdCm(thresholds, AlertLevel::Warning)) {
    return AlertLevel::Warning;
  }
  if (inputs.levelCm >= levelThresholdCm(thresholds, AlertLevel::Watch)) {
    return AlertLevel::Watch;
  }
  return AlertLevel::None;
}

}  // namespace wadiwatch
