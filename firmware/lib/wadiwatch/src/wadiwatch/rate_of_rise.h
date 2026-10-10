// Rate of rise: how fast the water level is changing, from the measurements of
// the last few minutes. Pure: the caller passes in the time and the levels.
#pragma once

#include <cstddef>
#include <cstdint>

namespace wadiwatch {

struct RateOfRiseConfig {
  // Length of the sliding window. Measurements older than this are forgotten.
  // Must be at least as long as the time a rise has to be sustained to raise an
  // alert (`AlertThresholds::warningRateOfRiseSustainMs`).
  uint32_t windowMs;
};

// Keeps the measurements of the sliding window. Fixed memory: it holds the
// newest kMaxMeasurements, which is more than a window of storm-mode
// measurements needs.
class RateOfRise {
 public:
  explicit RateOfRise(const RateOfRiseConfig& config);

  // A level measurement was taken. Times must not go backwards.
  void addMeasurement(uint64_t nowMs, float levelCm);
  // Forgets every measurement: the next one starts a new window.
  void clear();

  // The rate of rise over the window: from its oldest measurement to its newest.
  // Negative while the level falls; zero with fewer than two measurements.
  float rateCmPerMin() const;

  // The rate of rise that has been kept up for the whole of the last
  // `durationMs`: the slowest rise between two consecutive measurements in that
  // time. A rise that paused or fell back for one measurement was not kept up.
  // Zero when the measurements in the window do not reach back that far.
  float sustainedRateCmPerMin(uint32_t durationMs) const;

 private:
  enum { kMaxMeasurements = 16 };

  void dropOldest();

  uint32_t windowMs_;
  size_t count_ = 0;
  // Oldest first.
  uint64_t timesMs_[kMaxMeasurements];
  float levelsCm_[kMaxMeasurements];
};

}  // namespace wadiwatch
