// Direct tests of the rate of rise: the sliding window and what counts as a
// sustained rise, with no devices involved.
#include <cstdint>

#include <unity.h>

#include "wadiwatch/rate_of_rise.h"
#include "wadiwatch/site_config.h"

using wadiwatch::RateOfRise;

void setUp() {}
void tearDown() {}

namespace {

const float kMsPerMinute = 60.0f * 1000.0f;
// Rounding of the arithmetic, far below any rate that matters.
const float kRoundingCmPerMin = 0.01f;

const wadiwatch::WadiNodeConfig& node() { return wadiwatch::prototypeSite().wadiNode; }

// Time between two storm-mode measurements.
uint32_t intervalMs() { return node().mode.stormMeasureIntervalMs; }
uint32_t windowMs() { return node().rateOfRise.windowMs; }
uint32_t sustainMs() { return node().alertThresholds.warningRateOfRiseSustainMs; }

uint32_t intervalsIn(uint32_t durationMs) { return durationMs / intervalMs(); }

float perMinute(float cmPerInterval) {
  return cmPerInterval * kMsPerMinute / static_cast<float>(intervalMs());
}

// A wadi measured once per storm-mode interval, as the wadi node does.
class Wadi {
 public:
  Wadi() : rise(node().rateOfRise) {}

  // The next measurement finds the level changed by this much.
  void measureAfterRising(float cm) {
    if (measured_) nowMs_ += intervalMs();
    levelCm_ += cm;
    rise.addMeasurement(nowMs_, levelCm_);
    measured_ = true;
  }

  void measureAfterRising(float cmPerInterval, uint32_t intervals) {
    for (uint32_t i = 0; i < intervals; ++i) measureAfterRising(cmPerInterval);
  }

  // Some measurements could not be taken.
  void skipMeasurements(uint32_t intervals) { nowMs_ += static_cast<uint64_t>(intervals) * intervalMs(); }

  RateOfRise rise;

 private:
  uint64_t nowMs_ = 1000;
  float levelCm_ = 10.0f;
  bool measured_ = false;
};

}  // namespace

static void test_there_is_no_rate_of_rise_before_two_measurements() {
  Wadi wadi;
  TEST_ASSERT_EQUAL_FLOAT(0.0f, wadi.rise.rateCmPerMin());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, wadi.rise.sustainedRateCmPerMin(sustainMs()));

  wadi.measureAfterRising(0.0f);

  TEST_ASSERT_EQUAL_FLOAT(0.0f, wadi.rise.rateCmPerMin());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, wadi.rise.sustainedRateCmPerMin(sustainMs()));
}

static void test_a_steady_rise_is_reported_as_its_rate_per_minute() {
  Wadi wadi;
  const float stepCm = 2.0f;
  wadi.measureAfterRising(0.0f);

  for (uint32_t i = 0; i < 2 * intervalsIn(windowMs()); ++i) {
    wadi.measureAfterRising(stepCm);
    TEST_ASSERT_FLOAT_WITHIN(kRoundingCmPerMin, perMinute(stepCm), wadi.rise.rateCmPerMin());
  }
}

static void test_a_falling_level_is_a_negative_rate_of_rise() {
  Wadi wadi;
  const float stepCm = -1.5f;
  wadi.measureAfterRising(0.0f);

  wadi.measureAfterRising(stepCm, intervalsIn(windowMs()));

  TEST_ASSERT_FLOAT_WITHIN(kRoundingCmPerMin, perMinute(stepCm), wadi.rise.rateCmPerMin());
  TEST_ASSERT_FLOAT_WITHIN(kRoundingCmPerMin, perMinute(stepCm),
                           wadi.rise.sustainedRateCmPerMin(sustainMs()));
}

static void test_the_rate_of_rise_is_taken_from_the_oldest_to_the_newest_measurement_of_the_window() {
  Wadi wadi;
  const uint32_t windowIntervals = intervalsIn(windowMs());
  const float jumpCm = 10.0f;
  wadi.measureAfterRising(0.0f);

  // One jump, then a level that holds: the jump is spread over the measurements since.
  wadi.measureAfterRising(jumpCm);
  for (uint32_t since = 1; since < windowIntervals; ++since) {
    TEST_ASSERT_FLOAT_WITHIN(kRoundingCmPerMin, perMinute(jumpCm / static_cast<float>(since)),
                             wadi.rise.rateCmPerMin());
    wadi.measureAfterRising(0.0f);
  }
  TEST_ASSERT_FLOAT_WITHIN(kRoundingCmPerMin,
                           perMinute(jumpCm / static_cast<float>(windowIntervals)),
                           wadi.rise.rateCmPerMin());

  // One measurement later the level before the jump has left the window.
  wadi.measureAfterRising(0.0f);
  TEST_ASSERT_FLOAT_WITHIN(kRoundingCmPerMin, 0.0f, wadi.rise.rateCmPerMin());
}

static void test_a_rise_is_sustained_only_once_it_has_lasted_the_whole_duration() {
  Wadi wadi;
  const float stepCm = 6.0f;
  wadi.measureAfterRising(0.0f);

  for (uint32_t i = 1; i < intervalsIn(sustainMs()); ++i) {
    wadi.measureAfterRising(stepCm);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, wadi.rise.sustainedRateCmPerMin(sustainMs()));
  }
  wadi.measureAfterRising(stepCm);

  TEST_ASSERT_FLOAT_WITHIN(kRoundingCmPerMin, perMinute(stepCm),
                           wadi.rise.sustainedRateCmPerMin(sustainMs()));
}

static void test_the_sustained_rate_is_the_slowest_rise_between_measurements() {
  Wadi wadi;
  const float slowestCm = 3.0f;
  wadi.measureAfterRising(0.0f);
  wadi.measureAfterRising(9.0f, intervalsIn(sustainMs()) - 1);
  wadi.measureAfterRising(slowestCm);

  TEST_ASSERT_FLOAT_WITHIN(kRoundingCmPerMin, perMinute(slowestCm),
                           wadi.rise.sustainedRateCmPerMin(sustainMs()));

  // Once the slow stretch is older than the duration, it no longer holds the rate down.
  wadi.measureAfterRising(9.0f, intervalsIn(sustainMs()));
  TEST_ASSERT_FLOAT_WITHIN(kRoundingCmPerMin, perMinute(9.0f),
                           wadi.rise.sustainedRateCmPerMin(sustainMs()));
}

static void test_a_spike_followed_by_a_steady_level_is_not_a_sustained_rise() {
  Wadi wadi;
  wadi.measureAfterRising(0.0f);
  // As much in one measurement as a sustained rise brings in the whole duration.
  wadi.measureAfterRising(30.0f);

  for (uint32_t i = 0; i < 2 * intervalsIn(windowMs()); ++i) {
    wadi.measureAfterRising(0.0f);
    TEST_ASSERT_FLOAT_WITHIN(kRoundingCmPerMin, 0.0f, wadi.rise.sustainedRateCmPerMin(sustainMs()));
  }
}

static void test_a_rise_that_pauses_starts_its_duration_again() {
  Wadi wadi;
  const float stepCm = 6.0f;
  const uint32_t sustainIntervals = intervalsIn(sustainMs());
  wadi.measureAfterRising(0.0f);
  wadi.measureAfterRising(stepCm, sustainIntervals - 1);

  wadi.measureAfterRising(0.0f);

  for (uint32_t i = 1; i < sustainIntervals; ++i) {
    wadi.measureAfterRising(stepCm);
    TEST_ASSERT_FLOAT_WITHIN(kRoundingCmPerMin, 0.0f, wadi.rise.sustainedRateCmPerMin(sustainMs()));
  }
  wadi.measureAfterRising(stepCm);
  TEST_ASSERT_FLOAT_WITHIN(kRoundingCmPerMin, perMinute(stepCm),
                           wadi.rise.sustainedRateCmPerMin(sustainMs()));
}

static void test_a_missed_measurement_is_bridged_by_the_rise_across_the_gap() {
  Wadi wadi;
  const float stepCm = 6.0f;
  const uint32_t sustainIntervals = intervalsIn(sustainMs());
  TEST_ASSERT_TRUE(sustainIntervals >= 2);
  wadi.measureAfterRising(0.0f);

  // One measurement is missed while the wadi goes on rising at the same rate.
  wadi.skipMeasurements(1);
  wadi.measureAfterRising(2 * stepCm);
  wadi.measureAfterRising(stepCm, sustainIntervals - 2);

  TEST_ASSERT_FLOAT_WITHIN(kRoundingCmPerMin, perMinute(stepCm),
                           wadi.rise.sustainedRateCmPerMin(sustainMs()));
}

static void test_measurements_older_than_the_window_are_forgotten() {
  Wadi wadi;
  const float stepCm = 6.0f;
  wadi.measureAfterRising(0.0f);
  wadi.measureAfterRising(stepCm, intervalsIn(sustainMs()));

  // The sensor is fouled for longer than the window, then reads again.
  wadi.skipMeasurements(intervalsIn(windowMs()) + 1);
  wadi.measureAfterRising(stepCm);

  TEST_ASSERT_EQUAL_FLOAT(0.0f, wadi.rise.rateCmPerMin());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, wadi.rise.sustainedRateCmPerMin(sustainMs()));
}

static void test_clearing_forgets_every_measurement() {
  Wadi wadi;
  const float stepCm = 6.0f;
  wadi.measureAfterRising(0.0f);
  wadi.measureAfterRising(stepCm, intervalsIn(sustainMs()));

  wadi.rise.clear();
  wadi.measureAfterRising(stepCm);

  TEST_ASSERT_EQUAL_FLOAT(0.0f, wadi.rise.rateCmPerMin());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, wadi.rise.sustainedRateCmPerMin(sustainMs()));
}

static void test_more_measurements_than_it_can_hold_keeps_the_newest() {
  RateOfRise rise(node().rateOfRise);
  // Measured far more often than a wadi node does: a hundred times per window.
  const uint32_t stepMs = windowMs() / 100;
  const float cmPerStep = 0.1f;
  float levelCm = 0.0f;
  for (uint32_t i = 0; i <= 200; ++i) {
    rise.addMeasurement(static_cast<uint64_t>(i) * stepMs, levelCm);
    levelCm += cmPerStep;
  }

  TEST_ASSERT_FLOAT_WITHIN(kRoundingCmPerMin * 10.0f,
                           cmPerStep * kMsPerMinute / static_cast<float>(stepMs),
                           rise.rateCmPerMin());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_there_is_no_rate_of_rise_before_two_measurements);
  RUN_TEST(test_a_steady_rise_is_reported_as_its_rate_per_minute);
  RUN_TEST(test_a_falling_level_is_a_negative_rate_of_rise);
  RUN_TEST(test_the_rate_of_rise_is_taken_from_the_oldest_to_the_newest_measurement_of_the_window);
  RUN_TEST(test_a_rise_is_sustained_only_once_it_has_lasted_the_whole_duration);
  RUN_TEST(test_the_sustained_rate_is_the_slowest_rise_between_measurements);
  RUN_TEST(test_a_spike_followed_by_a_steady_level_is_not_a_sustained_rise);
  RUN_TEST(test_a_rise_that_pauses_starts_its_duration_again);
  RUN_TEST(test_a_missed_measurement_is_bridged_by_the_rise_across_the_gap);
  RUN_TEST(test_measurements_older_than_the_window_are_forgotten);
  RUN_TEST(test_clearing_forgets_every_measurement);
  RUN_TEST(test_more_measurements_than_it_can_hold_keeps_the_newest);
  return UNITY_END();
}
