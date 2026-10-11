// Scenario tests for the graded alert levels: what the wadi node puts on the air
// and what a bystander at the siren node hears and sees.
#include <cmath>
#include <vector>

#include <unity.h>

#include "wadiwatch/alert_engine.h"
#include "wadiwatch/packet.h"
#include "wadiwatch_testing/test_site.h"

using wadiwatch::AlertLevel;
using wadiwatch::AlertThresholds;
using wadiwatch::levelThresholdCm;
using wadiwatch::SiteConfig;
using wadiwatch::testing::SirenSound;
using wadiwatch::testing::TestSite;

void setUp() {}
void tearDown() {}

#define ASSERT_ALERT_LEVEL(expected, actual) \
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected), static_cast<uint8_t>(actual))

#define ASSERT_SIREN_SOUND(expected, actual) \
  TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual))

namespace {

float thresholdCm(const TestSite& site, AlertLevel alertLevel) {
  return levelThresholdCm(site.config.wadiNode.alertThresholds, alertLevel);
}

// A level that is clearly at `alertLevel` and clearly short of the next one up.
float levelWithin(const TestSite& site, AlertLevel alertLevel) {
  const float lowerCm = thresholdCm(site, alertLevel);
  const float upperCm = alertLevel == AlertLevel::Evacuate
                            ? site.config.wadiNode.alertThresholds.bankFullDepthCm
                            : thresholdCm(site, static_cast<AlertLevel>(
                                                    static_cast<uint8_t>(alertLevel) + 1));
  return (lowerCm + upperCm) / 2.0f;
}

// The longest the wadi node goes without measuring: one dry-mode interval.
// In storm mode it measures sooner.
uint64_t longestMeasureIntervalMs(const TestSite& site) {
  return site.config.wadiNode.mode.dryMeasureIntervalMs;
}

// Holds the wadi at one level until the wadi node has measured it, whichever
// operating mode the node is in: until its next ping of the level sensor.
void holdLevel(TestSite& site, float levelCm) {
  site.levelSensor.setWaterLevelCm(levelCm);
  const size_t readsBefore = site.levelSensor.readCount();
  for (uint64_t waited = 0; site.levelSensor.readCount() == readsBefore;
       waited += TestSite::kStepMs) {
    TEST_ASSERT_TRUE_MESSAGE(waited < longestMeasureIntervalMs(site),
                             "the wadi node never measured the level");
    site.runMs(TestSite::kStepMs);
  }
}

// The flood has passed: the wadi stays dry for longer than it takes a node with no
// alert to go back to dry mode and measure the dry bed more than once there. A node
// at Warning or above stays in storm mode and measures the dry bed every minute.
void stayDryLongAfterTheFlood(TestSite& site) {
  const wadiwatch::OperatingModeConfig& mode = site.config.wadiNode.mode;
  site.levelSensor.setWaterLevelCm(0.0f);
  site.runMs(static_cast<uint64_t>(mode.stormQuietMs) + 2ull * mode.dryMeasureIntervalMs);
}

}  // namespace

static void test_level_below_the_watch_threshold_puts_nothing_on_the_air() {
  TestSite site;
  site.levelSensor.setWaterLevelCm(thresholdCm(site, AlertLevel::Watch) / 2.0f);

  // Long enough for more than one dry-mode measurement; the first heartbeat is not due yet.
  site.runMs(2ull * site.config.wadiNode.mode.dryMeasureIntervalMs);

  TEST_ASSERT_EQUAL_UINT(0, site.air.transmitted().size());
  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
  TEST_ASSERT_FALSE(site.siren.isStrobeOn());
}

static void test_slow_flood_raises_watch_then_warning_then_evacuate_at_the_configured_levels() {
  TestSite site;
  const AlertThresholds& thresholds = site.config.wadiNode.alertThresholds;
  // The wadi rises by 1% of its bank-full depth per measurement, from a dry bed to bank-full:
  // a measurement every dry-mode interval at first, every storm-mode interval from Watch on.
  const float stepCm = thresholds.bankFullDepthCm / 100.0f;
  const int steps = 100;

  AlertLevel raised = AlertLevel::None;
  for (int step = 0; step <= steps; ++step) {
    const float levelCm = stepCm * static_cast<float>(step);
    holdLevel(site, levelCm);

    const AlertLevel onAir = site.latestAlertOnAir();
    if (onAir == raised) continue;

    // One level at a time, on the first measurement at or above its threshold.
    ASSERT_ALERT_LEVEL(static_cast<AlertLevel>(static_cast<uint8_t>(raised) + 1), onAir);
    TEST_ASSERT_TRUE(levelCm >= thresholdCm(site, onAir));
    TEST_ASSERT_TRUE(levelCm - stepCm < thresholdCm(site, onAir));
    raised = onAir;
  }

  ASSERT_ALERT_LEVEL(AlertLevel::Evacuate, raised);
}

static void test_siren_sound_follows_the_alert_level_through_a_slow_flood() {
  TestSite site;

  holdLevel(site, 0.0f);
  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());

  holdLevel(site, levelWithin(site, AlertLevel::Watch));
  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());

  holdLevel(site, levelWithin(site, AlertLevel::Warning));
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());

  holdLevel(site, levelWithin(site, AlertLevel::Evacuate));
  ASSERT_SIREN_SOUND(SirenSound::Continuous, site.listenToSiren());
}

static void test_watch_produces_no_siren_or_strobe_output() {
  TestSite site;
  site.levelSensor.setWaterLevelCm(levelWithin(site, AlertLevel::Watch));

  const uint64_t thirtyMinutesMs = 30ull * 60 * 1000;
  for (uint64_t elapsed = 0; elapsed < thirtyMinutesMs; elapsed += TestSite::kStepMs) {
    site.runMs(TestSite::kStepMs);
    TEST_ASSERT_FALSE(site.siren.isOn());
    TEST_ASSERT_FALSE(site.siren.isStrobeOn());
  }

  ASSERT_ALERT_LEVEL(AlertLevel::Watch, site.latestAlertOnAir());
}

static void test_warning_produces_the_intermittent_pattern_with_strobe() {
  TestSite site;
  const uint64_t onMs = site.config.sirenNode.intermittentOnMs;
  const uint64_t offMs = site.config.sirenNode.intermittentOffMs;
  site.levelSensor.setWaterLevelCm(levelWithin(site, AlertLevel::Warning));

  // Run up to the moment the siren first sounds.
  for (uint64_t waited = 0; !site.siren.isOn(); waited += TestSite::kStepMs) {
    TEST_ASSERT_TRUE_MESSAGE(waited < longestMeasureIntervalMs(site), "the siren never sounded");
    site.runMs(TestSite::kStepMs);
  }
  TEST_ASSERT_TRUE(site.siren.isStrobeOn());

  // From then on: sounding for the on time, silent for the off time, over and over,
  // with the strobe on throughout.
  const uint64_t periodMs = onMs + offMs;
  const uint64_t tenPeriodsMs = 10 * periodMs;
  for (uint64_t elapsed = TestSite::kStepMs; elapsed < tenPeriodsMs; elapsed += TestSite::kStepMs) {
    site.runMs(TestSite::kStepMs);
    const bool soundingExpected = (elapsed % periodMs) < onMs;
    TEST_ASSERT_EQUAL(soundingExpected, site.siren.isOn());
    TEST_ASSERT_TRUE(site.siren.isStrobeOn());
  }

  ASSERT_ALERT_LEVEL(AlertLevel::Warning, site.latestAlertOnAir());
}

static void test_evacuate_produces_a_continuous_siren_with_strobe() {
  TestSite site;
  const uint64_t periodMs =
      site.config.sirenNode.intermittentOnMs + site.config.sirenNode.intermittentOffMs;
  holdLevel(site, levelWithin(site, AlertLevel::Evacuate));

  const uint64_t tenPeriodsMs = 10 * periodMs;
  for (uint64_t elapsed = 0; elapsed < tenPeriodsMs; elapsed += TestSite::kStepMs) {
    site.runMs(TestSite::kStepMs);
    TEST_ASSERT_TRUE(site.siren.isOn());
    TEST_ASSERT_TRUE(site.siren.isStrobeOn());
  }

  ASSERT_ALERT_LEVEL(AlertLevel::Evacuate, site.latestAlertOnAir());
}

static void test_warning_escalating_to_evacuate_changes_the_siren_to_continuous() {
  TestSite site;
  holdLevel(site, levelWithin(site, AlertLevel::Warning));
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());

  holdLevel(site, levelWithin(site, AlertLevel::Evacuate));

  ASSERT_SIREN_SOUND(SirenSound::Continuous, site.listenToSiren());
  TEST_ASSERT_TRUE(site.siren.isStrobeOn());
}

static void test_falling_level_does_not_lower_evacuate() {
  TestSite site;
  holdLevel(site, levelWithin(site, AlertLevel::Evacuate));
  ASSERT_ALERT_LEVEL(AlertLevel::Evacuate, site.latestAlertOnAir());

  // The flood passes: the level falls back through every threshold to a dry bed.
  holdLevel(site, levelWithin(site, AlertLevel::Warning));
  ASSERT_ALERT_LEVEL(AlertLevel::Evacuate, site.latestAlertOnAir());
  holdLevel(site, levelWithin(site, AlertLevel::Watch));
  ASSERT_ALERT_LEVEL(AlertLevel::Evacuate, site.latestAlertOnAir());
  stayDryLongAfterTheFlood(site);

  ASSERT_ALERT_LEVEL(AlertLevel::Evacuate, site.latestAlertOnAir());
  ASSERT_SIREN_SOUND(SirenSound::Continuous, site.listenToSiren());
  TEST_ASSERT_TRUE(site.siren.isStrobeOn());
}

static void test_falling_level_does_not_lower_warning() {
  TestSite site;
  holdLevel(site, levelWithin(site, AlertLevel::Warning));

  stayDryLongAfterTheFlood(site);

  ASSERT_ALERT_LEVEL(AlertLevel::Warning, site.latestAlertOnAir());
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());
  TEST_ASSERT_TRUE(site.siren.isStrobeOn());
}

static void test_alert_level_still_escalates_after_the_level_has_fallen_and_risen_again() {
  TestSite site;
  holdLevel(site, levelWithin(site, AlertLevel::Warning));
  holdLevel(site, 0.0f);

  holdLevel(site, levelWithin(site, AlertLevel::Evacuate));

  ASSERT_ALERT_LEVEL(AlertLevel::Evacuate, site.latestAlertOnAir());
  ASSERT_SIREN_SOUND(SirenSound::Continuous, site.listenToSiren());
}

static void test_an_alert_carries_the_level_that_raised_it() {
  TestSite site;
  const float levelCm = levelWithin(site, AlertLevel::Warning);

  holdLevel(site, levelCm);

  const std::vector<wadiwatch::Packet> alerts = site.packetsOnAir(wadiwatch::PacketKind::Alert);
  TEST_ASSERT_EQUAL_UINT(1, alerts.size());
  ASSERT_ALERT_LEVEL(AlertLevel::Warning, alerts[0].alert.alertLevel);
  TEST_ASSERT_EQUAL_UINT16(site.config.wadiNode.id, alerts[0].sender);
  // Packets carry the level in whole millimetres.
  TEST_ASSERT_UINT16_WITHIN(1, static_cast<uint16_t>(std::lround(levelCm * 10.0f)),
                            alerts[0].alert.levelMm);
}

static void test_thresholds_and_bank_full_depth_come_from_the_wadi_node_configuration() {
  // The same wadi node moved to a channel half as deep, with the same percentages.
  SiteConfig shallower = wadiwatch::prototypeSite();
  shallower.wadiNode.alertThresholds.bankFullDepthCm /= 2.0f;
  TestSite shallowSite(shallower);
  TestSite prototypeSite;

  // The shallow channel's Watch threshold is half-way to the prototype's.
  const float levelCm = thresholdCm(shallowSite, AlertLevel::Watch);
  holdLevel(shallowSite, levelCm);
  holdLevel(prototypeSite, levelCm);

  ASSERT_ALERT_LEVEL(AlertLevel::Watch, shallowSite.latestAlertOnAir());
  ASSERT_ALERT_LEVEL(AlertLevel::None, prototypeSite.latestAlertOnAir());
}

static void test_each_threshold_is_configured_separately() {
  // A wadi committee raises this node's Warning threshold to where Evacuate used to be.
  SiteConfig recalibrated = wadiwatch::prototypeSite();
  AlertThresholds& thresholds = recalibrated.wadiNode.alertThresholds;
  const float formerEvacuatePercent = thresholds.evacuateLevelPercent;
  thresholds.evacuateLevelPercent = (formerEvacuatePercent + 100.0f) / 2.0f;
  thresholds.warningLevelPercent = formerEvacuatePercent;
  TestSite site(recalibrated);
  TestSite prototypeSite;

  // What was Warning at the prototype's thresholds is now only Watch.
  holdLevel(site, levelWithin(prototypeSite, AlertLevel::Warning));
  ASSERT_ALERT_LEVEL(AlertLevel::Watch, site.latestAlertOnAir());

  holdLevel(site, thresholdCm(site, AlertLevel::Warning));
  ASSERT_ALERT_LEVEL(AlertLevel::Warning, site.latestAlertOnAir());

  holdLevel(site, thresholdCm(site, AlertLevel::Evacuate));
  ASSERT_ALERT_LEVEL(AlertLevel::Evacuate, site.latestAlertOnAir());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_level_below_the_watch_threshold_puts_nothing_on_the_air);
  RUN_TEST(test_slow_flood_raises_watch_then_warning_then_evacuate_at_the_configured_levels);
  RUN_TEST(test_siren_sound_follows_the_alert_level_through_a_slow_flood);
  RUN_TEST(test_watch_produces_no_siren_or_strobe_output);
  RUN_TEST(test_warning_produces_the_intermittent_pattern_with_strobe);
  RUN_TEST(test_evacuate_produces_a_continuous_siren_with_strobe);
  RUN_TEST(test_warning_escalating_to_evacuate_changes_the_siren_to_continuous);
  RUN_TEST(test_falling_level_does_not_lower_evacuate);
  RUN_TEST(test_falling_level_does_not_lower_warning);
  RUN_TEST(test_alert_level_still_escalates_after_the_level_has_fallen_and_risen_again);
  RUN_TEST(test_an_alert_carries_the_level_that_raised_it);
  RUN_TEST(test_thresholds_and_bank_full_depth_come_from_the_wadi_node_configuration);
  RUN_TEST(test_each_threshold_is_configured_separately);
  return UNITY_END();
}
