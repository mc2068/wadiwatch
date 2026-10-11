#include "wadiwatch/level_measurement.h"

#include <cmath>

namespace wadiwatch {

namespace {

constexpr float kSpeedOfSoundAtFreezingMps = 331.3f;
constexpr float kFreezingKelvin = 273.15f;

// The middle of the distances; between the two middle ones when their number is even.
// Sorts the distances in place.
float median(float* distancesCm, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const float distanceCm = distancesCm[i];
    size_t j = i;
    for (; j > 0 && distancesCm[j - 1] > distanceCm; --j) distancesCm[j] = distancesCm[j - 1];
    distancesCm[j] = distanceCm;
  }
  const size_t middle = count / 2;
  if (count % 2 == 1) return distancesCm[middle];
  return (distancesCm[middle - 1] + distancesCm[middle]) / 2.0f;
}

}  // namespace

float speedOfSoundMps(float temperatureC) {
  return kSpeedOfSoundAtFreezingMps * std::sqrt(1.0f + temperatureC / kFreezingKelvin);
}

LevelMeasurement measureLevel(const Ping (&pings)[kPingsPerMeasurement],
                              const AirTemperature& temperature,
                              float mountingHeightCm,
                              const LevelSensorConfig& config) {
  // The sensor times the echo and works out the distance for its reference
  // temperature; the real distance is in proportion to the real speed of sound.
  const float correction = temperature.available
                               ? speedOfSoundMps(temperature.celsius) /
                                     speedOfSoundMps(config.referenceTemperatureC)
                               : 1.0f;
  const float nearestCm = config.minDistanceCm;
  const float farthestCm = mountingHeightCm + config.bedToleranceCm;

  float possibleCm[kPingsPerMeasurement];
  size_t possibleCount = 0;
  for (const Ping& ping : pings) {
    if (!ping.echoed) continue;
    const float distanceCm = ping.distanceCm * correction;
    // Written so that a reading that is not a number is discarded too.
    if (distanceCm >= nearestCm && distanceCm <= farthestCm) {
      possibleCm[possibleCount++] = distanceCm;
    }
  }

  LevelMeasurement measurement;
  measurement.discardedPings = static_cast<uint8_t>(kPingsPerMeasurement - possibleCount);
  measurement.hasLevel = possibleCount >= kMinPingsForLevel;
  measurement.levelCm =
      measurement.hasLevel ? mountingHeightCm - median(possibleCm, possibleCount) : 0.0f;
  return measurement;
}

}  // namespace wadiwatch
