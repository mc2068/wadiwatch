// Direct tests of the level measurement: the median of the pings, the physically
// possible range and the temperature correction, with no devices involved.
#include <cstddef>
#include <vector>

#include <unity.h>

#include "wadiwatch/level_measurement.h"
#include "wadiwatch/site_config.h"

using wadiwatch::AirTemperature;
using wadiwatch::kMinPingsForLevel;
using wadiwatch::kPingsPerMeasurement;
using wadiwatch::LevelMeasurement;
using wadiwatch::LevelSensorConfig;
using wadiwatch::measureLevel;
using wadiwatch::Ping;
using wadiwatch::speedOfSoundMps;

void setUp() {}
void tearDown() {}

namespace {

// Smaller than any difference the level sensor could resolve.
const float kJustCm = 0.1f;
// Rounding of the arithmetic, far below what the sensor resolves.
const float kRoundingCm = 0.01f;

const wadiwatch::WadiNodeConfig& node() { return wadiwatch::prototypeSite().wadiNode; }

// The distance the sensor reads when the water stands at this level.
float distanceToLevelCm(float levelCm) { return node().mountingHeightCm - levelCm; }

// A distance somewhere in the middle of what the sensor can physically report.
float possibleDistanceCm() { return (node().levelSensor.minDistanceCm + node().mountingHeightCm) / 2.0f; }

// Nearer than the sensor's blind zone lets it measure.
float tooNearCm() { return node().levelSensor.minDistanceCm - kJustCm; }

// Farther than the wadi bed can be.
float tooFarCm() {
  return node().mountingHeightCm + node().levelSensor.bedToleranceCm + kJustCm;
}

const float kNoEcho = -1.0f;

// One measurement's pings. A distance of kNoEcho stands for a ping that gave no reading.
struct Pings {
  explicit Pings(const std::vector<float>& distancesCm) {
    TEST_ASSERT_EQUAL_UINT(kPingsPerMeasurement, distancesCm.size());
    for (size_t i = 0; i < kPingsPerMeasurement; ++i) {
      pings[i].echoed = distancesCm[i] != kNoEcho;
      pings[i].distanceCm = distancesCm[i];
    }
  }
  Ping pings[kPingsPerMeasurement];
};

AirTemperature unknownTemperature() {
  const AirTemperature temperature = {false, 0.0f};
  return temperature;
}

AirTemperature airAt(float celsius) {
  const AirTemperature temperature = {true, celsius};
  return temperature;
}

LevelMeasurement measure(const std::vector<float>& distancesCm,
                         const AirTemperature& temperature = unknownTemperature()) {
  const Pings pings(distancesCm);
  return measureLevel(pings.pings, temperature, node().mountingHeightCm, node().levelSensor);
}

struct MedianRow {
  const char* description;
  std::vector<float> distancesCm;
  float expectedDistanceCm;
};

}  // namespace

static void test_five_agreeing_pings_give_the_mounting_height_minus_their_distance() {
  const float distanceCm = possibleDistanceCm();

  const LevelMeasurement measurement =
      measure({distanceCm, distanceCm, distanceCm, distanceCm, distanceCm});

  TEST_ASSERT_TRUE(measurement.hasLevel);
  TEST_ASSERT_FLOAT_WITHIN(kRoundingCm, node().mountingHeightCm - distanceCm, measurement.levelCm);
  TEST_ASSERT_EQUAL_UINT8(0, measurement.discardedPings);
}

static void test_the_level_is_the_median_of_the_pings() {
  const float d = possibleDistanceCm();
  // Something between the sensor and the water, and an echo that took a longer way round:
  // both physically possible distances.
  const float debrisCm = node().levelSensor.minDistanceCm + kJustCm;
  const float strayCm = node().mountingHeightCm;
  const std::vector<MedianRow> rows = {
      {"an outlier on the first ping", {debrisCm, d, d, d, d}, d},
      {"an outlier on the middle ping", {d, d, debrisCm, d, d}, d},
      {"an outlier on the last ping", {d, d, d, d, strayCm}, d},
      {"one outlier each way", {strayCm, d, d, debrisCm, d}, d},
      {"two outliers the same way", {debrisCm, d, d, debrisCm, d}, d},
      {"pings scattered around the water surface", {d + 2, d - 1, d, d + 1, d - 2}, d},
      {"pings in falling order", {d + 2, d + 1, d, d - 1, d - 2}, d},
  };

  for (const MedianRow& row : rows) {
    const LevelMeasurement measurement = measure(row.distancesCm);
    TEST_ASSERT_TRUE_MESSAGE(measurement.hasLevel, row.description);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(kRoundingCm, node().mountingHeightCm - row.expectedDistanceCm,
                                     measurement.levelCm, row.description);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, measurement.discardedPings, row.description);
  }
}

static void test_pings_outside_the_physically_possible_range_are_discarded_and_counted() {
  const float d = possibleDistanceCm();

  const LevelMeasurement measurement = measure({tooNearCm(), d, tooFarCm(), d, d});

  TEST_ASSERT_TRUE(measurement.hasLevel);
  TEST_ASSERT_FLOAT_WITHIN(kRoundingCm, node().mountingHeightCm - d, measurement.levelCm);
  TEST_ASSERT_EQUAL_UINT8(2, measurement.discardedPings);
}

static void test_an_impossible_ping_takes_no_part_in_the_median() {
  const float d = possibleDistanceCm();
  const float debrisCm = node().levelSensor.minDistanceCm + kJustCm;

  // Were the two impossible pings counted as near readings, the debris would be the median.
  const LevelMeasurement measurement = measure({tooNearCm(), tooNearCm(), debrisCm, d, d});

  TEST_ASSERT_TRUE(measurement.hasLevel);
  TEST_ASSERT_FLOAT_WITHIN(kRoundingCm, node().mountingHeightCm - d, measurement.levelCm);
  TEST_ASSERT_EQUAL_UINT8(2, measurement.discardedPings);
}

static void test_pings_at_the_edges_of_the_possible_range_are_kept() {
  const float nearestCm = node().levelSensor.minDistanceCm;
  const float farthestCm = node().mountingHeightCm + node().levelSensor.bedToleranceCm;

  const LevelMeasurement nearest =
      measure({nearestCm, nearestCm, nearestCm, nearestCm, nearestCm});
  const LevelMeasurement farthest =
      measure({farthestCm, farthestCm, farthestCm, farthestCm, farthestCm});

  TEST_ASSERT_TRUE(nearest.hasLevel);
  TEST_ASSERT_EQUAL_UINT8(0, nearest.discardedPings);
  TEST_ASSERT_TRUE(farthest.hasLevel);
  TEST_ASSERT_EQUAL_UINT8(0, farthest.discardedPings);
  // A reading a little beyond the bed is a level a little below zero.
  TEST_ASSERT_FLOAT_WITHIN(kRoundingCm, -node().levelSensor.bedToleranceCm, farthest.levelCm);
}

static void test_a_ping_without_an_echo_is_discarded_and_counted() {
  const float d = possibleDistanceCm();

  const LevelMeasurement measurement = measure({d, kNoEcho, d, d, kNoEcho});

  TEST_ASSERT_TRUE(measurement.hasLevel);
  TEST_ASSERT_FLOAT_WITHIN(kRoundingCm, node().mountingHeightCm - d, measurement.levelCm);
  TEST_ASSERT_EQUAL_UINT8(2, measurement.discardedPings);
}

static void test_with_four_pings_left_the_level_lies_between_the_two_middle_ones() {
  const float d = possibleDistanceCm();

  const LevelMeasurement measurement = measure({d + 3, tooFarCm(), d - 3, d + 1, d - 1});

  TEST_ASSERT_TRUE(measurement.hasLevel);
  TEST_ASSERT_FLOAT_WITHIN(kRoundingCm, node().mountingHeightCm - d, measurement.levelCm);
  TEST_ASSERT_EQUAL_UINT8(1, measurement.discardedPings);
}

static void test_with_fewer_than_a_majority_of_pings_left_there_is_no_level() {
  const float d = possibleDistanceCm();
  std::vector<float> distancesCm(kPingsPerMeasurement, tooNearCm());
  // One ping short of a majority is possible.
  for (size_t i = 0; i + 1 < kMinPingsForLevel; ++i) distancesCm[i] = d;

  const LevelMeasurement measurement = measure(distancesCm);

  TEST_ASSERT_FALSE(measurement.hasLevel);
  TEST_ASSERT_EQUAL_UINT8(kPingsPerMeasurement - (kMinPingsForLevel - 1),
                          measurement.discardedPings);
}

static void test_a_sensor_that_returns_nothing_gives_no_level_and_every_ping_is_counted() {
  const LevelMeasurement measurement = measure({kNoEcho, kNoEcho, kNoEcho, kNoEcho, kNoEcho});

  TEST_ASSERT_FALSE(measurement.hasLevel);
  TEST_ASSERT_EQUAL_UINT8(kPingsPerMeasurement, measurement.discardedPings);
}

static void test_speed_of_sound_matches_published_values() {
  // Speed of sound in dry air, metres per second.
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 325.2f, speedOfSoundMps(-10.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 331.3f, speedOfSoundMps(0.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 343.2f, speedOfSoundMps(20.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 354.7f, speedOfSoundMps(40.0f));
}

static void test_distance_is_corrected_for_air_temperature_when_a_reading_is_available() {
  const float trueDistanceCm = possibleDistanceCm();
  const float hotC = 40.0f;
  // Sound is faster in hot air: the echo is back sooner, and a sensor that works
  // out distance for its reference temperature reports the water nearer than it is.
  const float reportedCm = trueDistanceCm * speedOfSoundMps(node().levelSensor.referenceTemperatureC) /
                           speedOfSoundMps(hotC);
  TEST_ASSERT_TRUE(trueDistanceCm - reportedCm > 1.0f);
  const std::vector<float> pings(kPingsPerMeasurement, reportedCm);

  const LevelMeasurement corrected = measure(pings, airAt(hotC));
  const LevelMeasurement uncorrected = measure(pings, unknownTemperature());

  TEST_ASSERT_TRUE(corrected.hasLevel);
  TEST_ASSERT_FLOAT_WITHIN(kJustCm, node().mountingHeightCm - trueDistanceCm, corrected.levelCm);
  TEST_ASSERT_TRUE(uncorrected.hasLevel);
  TEST_ASSERT_FLOAT_WITHIN(kRoundingCm, node().mountingHeightCm - reportedCm, uncorrected.levelCm);
}

static void test_at_the_reference_temperature_the_correction_changes_nothing() {
  const float d = possibleDistanceCm();
  const std::vector<float> pings(kPingsPerMeasurement, d);

  const LevelMeasurement measurement =
      measure(pings, airAt(node().levelSensor.referenceTemperatureC));

  TEST_ASSERT_TRUE(measurement.hasLevel);
  TEST_ASSERT_FLOAT_WITHIN(kRoundingCm, node().mountingHeightCm - d, measurement.levelCm);
}

static void test_the_possible_range_applies_to_the_corrected_distance() {
  const float freezingC = 0.0f;
  // In cold air the sensor reports the dry bed farther away than it is: here, out of range.
  const float reportedCm = tooFarCm();
  const float correctedCm = reportedCm * speedOfSoundMps(freezingC) /
                            speedOfSoundMps(node().levelSensor.referenceTemperatureC);
  TEST_ASSERT_TRUE(correctedCm <
                   node().mountingHeightCm + node().levelSensor.bedToleranceCm - kJustCm);
  const std::vector<float> pings(kPingsPerMeasurement, reportedCm);

  const LevelMeasurement corrected = measure(pings, airAt(freezingC));
  const LevelMeasurement uncorrected = measure(pings, unknownTemperature());

  TEST_ASSERT_TRUE(corrected.hasLevel);
  TEST_ASSERT_EQUAL_UINT8(0, corrected.discardedPings);
  TEST_ASSERT_FALSE(uncorrected.hasLevel);
  TEST_ASSERT_EQUAL_UINT8(kPingsPerMeasurement, uncorrected.discardedPings);
}

static void test_cold_air_over_a_dry_bed_is_within_the_bed_tolerance_without_a_temperature_reading() {
  // The coldest night the prototype site is set up for.
  const float coldC = -5.0f;
  const float reportedCm = distanceToLevelCm(0.0f) *
                           speedOfSoundMps(node().levelSensor.referenceTemperatureC) /
                           speedOfSoundMps(coldC);
  const std::vector<float> pings(kPingsPerMeasurement, reportedCm);

  const LevelMeasurement measurement = measure(pings, unknownTemperature());

  TEST_ASSERT_TRUE(measurement.hasLevel);
  TEST_ASSERT_EQUAL_UINT8(0, measurement.discardedPings);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_five_agreeing_pings_give_the_mounting_height_minus_their_distance);
  RUN_TEST(test_the_level_is_the_median_of_the_pings);
  RUN_TEST(test_pings_outside_the_physically_possible_range_are_discarded_and_counted);
  RUN_TEST(test_an_impossible_ping_takes_no_part_in_the_median);
  RUN_TEST(test_pings_at_the_edges_of_the_possible_range_are_kept);
  RUN_TEST(test_a_ping_without_an_echo_is_discarded_and_counted);
  RUN_TEST(test_with_four_pings_left_the_level_lies_between_the_two_middle_ones);
  RUN_TEST(test_with_fewer_than_a_majority_of_pings_left_there_is_no_level);
  RUN_TEST(test_a_sensor_that_returns_nothing_gives_no_level_and_every_ping_is_counted);
  RUN_TEST(test_speed_of_sound_matches_published_values);
  RUN_TEST(test_distance_is_corrected_for_air_temperature_when_a_reading_is_available);
  RUN_TEST(test_at_the_reference_temperature_the_correction_changes_nothing);
  RUN_TEST(test_the_possible_range_applies_to_the_corrected_distance);
  RUN_TEST(test_cold_air_over_a_dry_bed_is_within_the_bed_tolerance_without_a_temperature_reading);
  return UNITY_END();
}
