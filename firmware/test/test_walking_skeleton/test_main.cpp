// Scenario tests for the life-safety path: a wadi node, the radio and a siren node.
#include <unity.h>

#include "wadiwatch/alert_engine.h"
#include "wadiwatch_testing/test_site.h"

using wadiwatch::AlertLevel;
using wadiwatch::testing::SirenSound;
using wadiwatch::testing::TestSite;

void setUp() {}
void tearDown() {}

static float warningLevelCm(const TestSite& site) {
  return wadiwatch::levelThresholdCm(site.config.wadiNode.alertThresholds, AlertLevel::Warning);
}

// Whether the siren sounds at any moment of the next `durationMs`. At Warning it
// comes and goes, so it is listened to at every step.
static bool sirenSoundsWithin(TestSite& site, uint64_t durationMs) {
  bool sounded = false;
  for (uint64_t elapsed = 0; elapsed < durationMs; elapsed += TestSite::kStepMs) {
    site.runMs(TestSite::kStepMs);
    sounded = sounded || site.siren.isOn();
  }
  return sounded;
}

static void test_level_below_warning_threshold_produces_no_siren_output() {
  TestSite site;
  site.levelSensor.setWaterLevelCm(warningLevelCm(site) * 0.8f);

  site.runMinutes(30);

  // This level is above the Watch threshold, so a Watch alert is on the air; Watch never sounds the siren.
  TEST_ASSERT_FALSE(site.siren.isOn());
  TEST_ASSERT_FALSE(site.siren.isStrobeOn());
}

static void test_level_crossing_warning_threshold_turns_siren_on() {
  TestSite site;
  const float warningCm = warningLevelCm(site);

  site.levelSensor.setWaterLevelCm(warningCm * 0.5f);
  site.runMinutes(10);
  TEST_ASSERT_FALSE(site.siren.isOn());

  // It starts to rain, which puts the node in storm mode, and the wadi rises by
  // 10% of the threshold each minute until it is well past it.
  site.rainGauge.tip();
  for (float fraction = 0.6f; fraction < 1.25f; fraction += 0.1f) {
    site.levelSensor.setWaterLevelCm(warningCm * fraction);
    site.runMinutes(1);
  }

  TEST_ASSERT_TRUE(site.listenToSiren() != SirenSound::Silent);
}

static void test_in_dry_mode_siren_sounds_within_one_measurement_interval_of_the_crossing() {
  TestSite site;
  site.levelSensor.setWaterLevelCm(0);
  // The wadi is dry and there is no rain, so the node is in dry mode.
  site.runMinutes(5);

  site.levelSensor.setWaterLevelCm(warningLevelCm(site));

  TEST_ASSERT_TRUE(sirenSoundsWithin(site, site.config.wadiNode.mode.dryMeasureIntervalMs));
}

static void test_in_storm_mode_siren_sounds_within_one_measurement_interval_of_the_crossing() {
  TestSite site;
  site.levelSensor.setWaterLevelCm(0);
  // It has started to rain: the first tip of the rain gauge puts the node in storm mode.
  site.rainGauge.tip();
  site.runMinutes(5);

  site.levelSensor.setWaterLevelCm(warningLevelCm(site));

  TEST_ASSERT_TRUE(sirenSoundsWithin(site, site.config.wadiNode.mode.stormMeasureIntervalMs));
}

static void test_malformed_frame_does_not_sound_siren() {
  TestSite site;

  site.air.inject({0xFF, 0x00, 0x13, 0x37});
  site.air.inject({});
  site.runMinutes(1);

  TEST_ASSERT_FALSE(site.siren.isOn());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_level_below_warning_threshold_produces_no_siren_output);
  RUN_TEST(test_level_crossing_warning_threshold_turns_siren_on);
  RUN_TEST(test_in_dry_mode_siren_sounds_within_one_measurement_interval_of_the_crossing);
  RUN_TEST(test_in_storm_mode_siren_sounds_within_one_measurement_interval_of_the_crossing);
  RUN_TEST(test_malformed_frame_does_not_sound_siren);
  return UNITY_END();
}
