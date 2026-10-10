// Level measurement: turns the ultrasonic pings of one measurement into a water level.
// Pure: no hardware, no clock, no memory of earlier measurements.
#pragma once

#include <cstddef>
#include <cstdint>

namespace wadiwatch {

// One measurement is the median of this many pings.
constexpr size_t kPingsPerMeasurement = 5;
// A level needs a majority of the pings behind it, so that the median still
// outvotes a single outlier among the pings that are left.
constexpr size_t kMinPingsForLevel = kPingsPerMeasurement / 2 + 1;

// The level sensor fitted to a wadi node, and what it can physically report there.
struct LevelSensorConfig {
  // The sensor cannot measure anything nearer than this (its blind zone).
  float minDistanceCm;
  // How far below the wadi bed a reading may fall and still be a reading of the
  // bed. Must cover sensor noise and, on a node without a temperature reading,
  // the error cold air adds over the whole mounting height.
  float bedToleranceCm;
  // Air temperature for which the sensor works out its distances.
  float referenceTemperatureC;
};

// One ultrasonic ping as the level sensor reported it.
struct Ping {
  // False when the sensor gave no reading (no echo came back).
  bool echoed;
  // Distance from the sensor face down to whatever returned the echo.
  float distanceCm;
};

// The air temperature at the time of the measurement, when the node could read it.
struct AirTemperature {
  bool available;
  float celsius;
};

struct LevelMeasurement {
  // False when too few pings were left to trust a level.
  bool hasLevel;
  // Water level above the wadi bed. A little below zero over a dry bed is normal.
  float levelCm;
  // Pings left out of the median: no echo, or a distance that is physically
  // impossible at this site.
  uint8_t discardedPings;
};

// Speed of sound in air at this temperature.
float speedOfSoundMps(float temperatureC);

// The level the pings of one measurement give: each ping is corrected for the
// air temperature when it is available, pings outside the physically possible
// range (nearer than the blind zone, or beyond the bed) are discarded, and the
// level is the mounting height minus the median distance of the rest.
LevelMeasurement measureLevel(const Ping (&pings)[kPingsPerMeasurement],
                              const AirTemperature& temperature,
                              float mountingHeightCm,
                              const LevelSensorConfig& config);

}  // namespace wadiwatch
