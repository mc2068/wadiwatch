// Scenario tests for the life-safety path: a wadi node, the radio and a siren node.
#include <unity.h>

#include "wadiwatch_testing/test_site.h"

using wadiwatch::testing::TestSite;

void setUp() {}
void tearDown() {}

static void test_level_below_warning_threshold_produces_no_siren_output() {
  TestSite site;
  site.levelSensor.setWaterLevelCm(site.config.wadiNode.warningLevelCm * 0.8f);

  site.runMinutes(30);

  // This level is above the Watch threshold, so a Watch alert is on the air; Watch never sounds the siren.
  TEST_ASSERT_FALSE(site.siren.isOn());
}

static void test_level_crossing_warning_threshold_turns_siren_on() {
  TestSite site;
  const float warningLevelCm = site.config.wadiNode.warningLevelCm;

  site.levelSensor.setWaterLevelCm(warningLevelCm * 0.5f);
  site.runMinutes(10);
  TEST_ASSERT_FALSE(site.siren.isOn());

  // The wadi rises by 10% of the threshold each minute until it is well past it.
  for (float fraction = 0.6f; fraction < 1.25f; fraction += 0.1f) {
    site.levelSensor.setWaterLevelCm(warningLevelCm * fraction);
    site.runMinutes(1);
  }

  TEST_ASSERT_TRUE(site.siren.isOn());
}

static void test_siren_sounds_within_one_measurement_interval_of_the_crossing() {
  TestSite site;
  site.levelSensor.setWaterLevelCm(0);
  site.runMinutes(5);

  site.levelSensor.setWaterLevelCm(site.config.wadiNode.warningLevelCm);
  // The wadi is dry and there is no rain, so the node is in dry mode.
  site.runMs(site.config.wadiNode.mode.dryMeasureIntervalMs);

  TEST_ASSERT_TRUE(site.siren.isOn());
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
