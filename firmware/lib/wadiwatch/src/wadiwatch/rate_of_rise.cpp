#include "wadiwatch/rate_of_rise.h"

namespace wadiwatch {

namespace {

constexpr float kMsPerMinute = 60.0f * 1000.0f;

float cmPerMin(float riseCm, uint64_t elapsedMs) {
  return riseCm * kMsPerMinute / static_cast<float>(elapsedMs);
}

}  // namespace

RateOfRise::RateOfRise(const RateOfRiseConfig& config) : windowMs_(config.windowMs) {}

void RateOfRise::addMeasurement(uint64_t nowMs, float levelCm) {
  // A second measurement at the same instant replaces the first.
  if (count_ > 0 && timesMs_[count_ - 1] >= nowMs) --count_;
  while (count_ > 0 && nowMs - timesMs_[0] > windowMs_) dropOldest();
  if (count_ == kMaxMeasurements) dropOldest();
  timesMs_[count_] = nowMs;
  levelsCm_[count_] = levelCm;
  ++count_;
}

void RateOfRise::clear() { count_ = 0; }

float RateOfRise::rateCmPerMin() const {
  if (count_ < 2) return 0.0f;
  const size_t newest = count_ - 1;
  return cmPerMin(levelsCm_[newest] - levelsCm_[0], timesMs_[newest] - timesMs_[0]);
}

float RateOfRise::sustainedRateCmPerMin(uint32_t durationMs) const {
  if (count_ < 2) return 0.0f;
  const size_t newest = count_ - 1;
  float slowest = 0.0f;
  // From the newest measurement back, until the duration is covered.
  for (size_t i = newest; i > 0; --i) {
    const float rate =
        cmPerMin(levelsCm_[i] - levelsCm_[i - 1], timesMs_[i] - timesMs_[i - 1]);
    if (i == newest || rate < slowest) slowest = rate;
    if (timesMs_[newest] - timesMs_[i - 1] >= durationMs) return slowest;
  }
  return 0.0f;
}

void RateOfRise::dropOldest() {
  for (size_t i = 1; i < count_; ++i) {
    timesMs_[i - 1] = timesMs_[i];
    levelsCm_[i - 1] = levelsCm_[i];
  }
  --count_;
}

}  // namespace wadiwatch
