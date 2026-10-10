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

static void test_level_below_warning_threshold_produces_no_siren_output() {
  TestSite site;
  site.levelSensor.setWaterLevelCm(warningLevelCm(site) * 0.8f);

  site.runMinutes(30);

  TEST_ASSERT_FALSE(site.siren.isOn());
  TEST_ASSERT_FALSE(site.siren.isStrobeOn());
}

static void test_level_crossing_warning_threshold_turns_siren_on() {
  TestSite site;
  const float warningCm = warningLevelCm(site);

  site.levelSensor.setWaterLevelCm(warningCm * 0.5f);
  site.runMinutes(10);
  TEST_ASSERT_FALSE(site.siren.isOn());

  // The wadi rises by 10% of the threshold each minute until it is well past it.
  for (float fraction = 0.6f; fraction < 1.25f; fraction += 0.1f) {
    site.levelSensor.setWaterLevelCm(warningCm * fraction);
    site.runMinutes(1);
  }

  TEST_ASSERT_TRUE(site.listenToSiren() != SirenSound::Silent);
}

static void test_siren_sounds_within_one_measurement_interval_of_the_crossing() {
  TestSite site;
  site.levelSensor.setWaterLevelCm(0);
  site.runMinutes(5);

  site.levelSensor.setWaterLevelCm(warningLevelCm(site));
  bool sounded = false;
  for (uint64_t elapsed = 0; elapsed < site.config.wadiNode.measureIntervalMs;
       elapsed += TestSite::kStepMs) {
    site.runMs(TestSite::kStepMs);
    sounded = sounded || site.siren.isOn();
  }

  TEST_ASSERT_TRUE(sounded);
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
  RUN_TEST(test_siren_sounds_within_one_measurement_interval_of_the_crossing);
  RUN_TEST(test_malformed_frame_does_not_sound_siren);
  return UNITY_END();
}
