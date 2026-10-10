// Table-driven tests of the alert engine: threshold boundaries, with no devices involved.
#include <cstddef>
#include <vector>

#include <unity.h>

#include "wadiwatch/alert_engine.h"
#include "wadiwatch/site_config.h"

using wadiwatch::AlertInputs;
using wadiwatch::AlertLevel;
using wadiwatch::AlertThresholds;
using wadiwatch::evaluateAlertLevel;
using wadiwatch::levelThresholdCm;

void setUp() {}
void tearDown() {}

namespace {

// Smaller than any difference the level sensor could resolve.
const float kJustCm = 0.1f;
// Less rain than one tip of a rain gauge.
const float kJustMm = 0.1f;

struct Row {
  const char* description;
  float levelCm;
  AlertLevel expected;
};

// A calm wadi apart from its level: no rise, no rain, nothing heard from upstream, no alert yet.
AlertInputs levelOnly(float levelCm) {
  const AlertInputs inputs = {levelCm, 0.0f, 0.0f, AlertLevel::None, AlertLevel::None};
  return inputs;
}

// What the rain calls for, on its own and together with the level.
struct RainRow {
  const char* description;
  float levelCm;
  float rainInWindowMm;
  AlertLevel expected;
};

// No rise, nothing heard from upstream and no alert yet.
AlertInputs levelAndRain(float levelCm, float rainInWindowMm) {
  const AlertInputs inputs = {levelCm, 0.0f, rainInWindowMm, AlertLevel::None, AlertLevel::None};
  return inputs;
}

// What the upstream node's alert level calls for, together with this node's own
// level and the alert level already in force here.
struct UpstreamRow {
  const char* description;
  float levelCm;
  AlertLevel alertLevelInForce;
  AlertLevel upstreamAlertLevel;
  AlertLevel expected;
};

std::vector<UpstreamRow> upstreamRows(const AlertThresholds& thresholds) {
  const float watchCm = levelThresholdCm(thresholds, AlertLevel::Watch);
  const float warningCm = levelThresholdCm(thresholds, AlertLevel::Warning);
  const float evacuateCm = levelThresholdCm(thresholds, AlertLevel::Evacuate);
  const AlertLevel none = AlertLevel::None;
  const AlertLevel watch = AlertLevel::Watch;
  const AlertLevel warning = AlertLevel::Warning;
  const AlertLevel evacuate = AlertLevel::Evacuate;
  return {
      // Warning upstream alone raises nothing here.
      {"dry bed, Warning upstream", 0.0f, none, warning, none},
      {"dry bed, Evacuate upstream", 0.0f, none, evacuate, none},
      {"just below Watch, Warning upstream", watchCm - kJustCm, none, warning, none},
      {"at Watch, Warning upstream", watchCm, none, warning, watch},
      {"at Watch, Evacuate upstream", watchCm, none, evacuate, watch},
      {"just below Warning, Warning upstream", warningCm - kJustCm, none, warning, watch},
      // Warning here needs Warning or above upstream to become Evacuate.
      {"at Warning, nothing heard upstream", warningCm, none, none, warning},
      {"at Warning, Watch upstream", warningCm, none, watch, warning},
      {"at Warning, Warning upstream", warningCm, none, warning, evacuate},
      {"at Warning, Evacuate upstream", warningCm, none, evacuate, evacuate},
      {"just below Evacuate, Warning upstream", evacuateCm - kJustCm, none, warning, evacuate},
      {"at Evacuate, nothing heard upstream", evacuateCm, none, none, evacuate},
      {"at Evacuate, Warning upstream", evacuateCm, none, warning, evacuate},
      // A Warning already in force here counts, wherever the level is by now.
      {"Warning in force, level back on a dry bed, Warning upstream", 0.0f, warning, warning,
       evacuate},
      {"Warning in force, level at Watch, Evacuate upstream", watchCm, warning, evacuate, evacuate},
      {"Warning in force, level at Warning, Warning upstream", warningCm, warning, warning,
       evacuate},
      {"Watch in force, level back on a dry bed, Warning upstream", 0.0f, watch, warning, none},
      {"Watch in force, level at Watch, Warning upstream", watchCm, watch, warning, watch},
      // Without the upstream node, the alert level in force changes nothing: the answer
      // is what the readings call for now, and holding the alert is the caller's job.
      {"Warning in force, dry bed, nothing heard upstream", 0.0f, warning, none, none},
      {"Warning in force, dry bed, Watch upstream", 0.0f, warning, watch, none},
      {"Evacuate in force, level at Watch, nothing heard upstream", watchCm, evacuate, none, watch},
  };
}

void assertUpstream(const AlertThresholds& thresholds) {
  for (const UpstreamRow& row : upstreamRows(thresholds)) {
    const AlertInputs inputs = {row.levelCm, 0.0f, 0.0f, row.upstreamAlertLevel,
                                row.alertLevelInForce};
    const AlertLevel actual = evaluateAlertLevel(inputs, thresholds);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(static_cast<uint8_t>(row.expected),
                                    static_cast<uint8_t>(actual), row.description);
  }
}

std::vector<RainRow> rainRows(const AlertThresholds& thresholds) {
  const float watchCm = levelThresholdCm(thresholds, AlertLevel::Watch);
  const float warningCm = levelThresholdCm(thresholds, AlertLevel::Warning);
  const float evacuateCm = levelThresholdCm(thresholds, AlertLevel::Evacuate);
  const float watchMm = thresholds.watchRainMm;
  return {
      {"no rain on a dry bed", 0.0f, 0.0f, AlertLevel::None},
      {"rain just short of Watch on a dry bed", 0.0f, watchMm - kJustMm, AlertLevel::None},
      {"exactly the Watch rain on a dry bed", 0.0f, watchMm, AlertLevel::Watch},
      {"rain just over Watch on a dry bed", 0.0f, watchMm + kJustMm, AlertLevel::Watch},
      {"a cloudburst on a dry bed is still only Watch", 0.0f, watchMm * 10.0f, AlertLevel::Watch},
      {"rain short of Watch, level just below Watch", watchCm - kJustCm, watchMm - kJustMm,
       AlertLevel::None},
      {"Watch rain, level just below Watch", watchCm - kJustCm, watchMm, AlertLevel::Watch},
      {"rain short of Watch, level at Watch", watchCm, watchMm - kJustMm, AlertLevel::Watch},
      {"Watch rain, level at Watch", watchCm, watchMm, AlertLevel::Watch},
      {"Watch rain, level just below Warning", warningCm - kJustCm, watchMm, AlertLevel::Watch},
      {"Watch rain does not hold back Warning", warningCm, watchMm, AlertLevel::Warning},
      {"Watch rain does not hold back Evacuate", evacuateCm, watchMm, AlertLevel::Evacuate},
  };
}

void assertRain(const AlertThresholds& thresholds) {
  for (const RainRow& row : rainRows(thresholds)) {
    const AlertLevel actual =
        evaluateAlertLevel(levelAndRain(row.levelCm, row.rainInWindowMm), thresholds);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(static_cast<uint8_t>(row.expected),
                                    static_cast<uint8_t>(actual), row.description);
  }
}

std::vector<Row> boundaryRows(const AlertThresholds& thresholds) {
  const float watchCm = levelThresholdCm(thresholds, AlertLevel::Watch);
  const float warningCm = levelThresholdCm(thresholds, AlertLevel::Warning);
  const float evacuateCm = levelThresholdCm(thresholds, AlertLevel::Evacuate);
  return {
      {"sensor reads below the bed", -kJustCm, AlertLevel::None},
      {"dry bed", 0.0f, AlertLevel::None},
      {"just below Watch", watchCm - kJustCm, AlertLevel::None},
      {"exactly at Watch", watchCm, AlertLevel::Watch},
      {"just above Watch", watchCm + kJustCm, AlertLevel::Watch},
      {"just below Warning", warningCm - kJustCm, AlertLevel::Watch},
      {"exactly at Warning", warningCm, AlertLevel::Warning},
      {"just above Warning", warningCm + kJustCm, AlertLevel::Warning},
      {"just below Evacuate", evacuateCm - kJustCm, AlertLevel::Warning},
      {"exactly at Evacuate", evacuateCm, AlertLevel::Evacuate},
      {"just above Evacuate", evacuateCm + kJustCm, AlertLevel::Evacuate},
      {"bank-full", thresholds.bankFullDepthCm, AlertLevel::Evacuate},
      {"over the banks", thresholds.bankFullDepthCm * 2.0f, AlertLevel::Evacuate},
  };
}

void assertBoundaries(const AlertThresholds& thresholds) {
  for (const Row& row : boundaryRows(thresholds)) {
    const AlertLevel actual = evaluateAlertLevel(levelOnly(row.levelCm), thresholds);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(static_cast<uint8_t>(row.expected),
                                    static_cast<uint8_t>(actual), row.description);
  }
}

// A second site with nothing in common with the prototype's starting values.
AlertThresholds shallowCulvert() {
  const AlertThresholds thresholds = {
      /* bankFullDepthCm */ 80.0f,
      /* watchLevelPercent */ 25.0f,
      /* warningLevelPercent */ 45.0f,
      /* evacuateLevelPercent */ 90.0f,
      /* watchRainMm */ 6.0f,
      /* watchRainWindowMs */ 20u * 60u * 1000u,
  };
  return thresholds;
}

}  // namespace

static void test_level_thresholds_are_percentages_of_bank_full_depth() {
  const AlertThresholds thresholds = shallowCulvert();

  TEST_ASSERT_EQUAL_FLOAT(20.0f, levelThresholdCm(thresholds, AlertLevel::Watch));
  TEST_ASSERT_EQUAL_FLOAT(36.0f, levelThresholdCm(thresholds, AlertLevel::Warning));
  TEST_ASSERT_EQUAL_FLOAT(72.0f, levelThresholdCm(thresholds, AlertLevel::Evacuate));
}

static void test_alert_level_at_each_threshold_boundary_of_the_prototype_site() {
  assertBoundaries(wadiwatch::prototypeSite().wadiNode.alertThresholds);
}

static void test_alert_level_at_each_threshold_boundary_of_another_site() {
  assertBoundaries(shallowCulvert());
}

static void test_rain_in_the_window_raises_watch_at_the_prototype_site() {
  assertRain(wadiwatch::prototypeSite().wadiNode.alertThresholds);
}

static void test_rain_in_the_window_raises_watch_at_another_site() {
  assertRain(shallowCulvert());
}

static void test_warning_here_and_upstream_calls_for_evacuate_at_the_prototype_site() {
  assertUpstream(wadiwatch::prototypeSite().wadiNode.alertThresholds);
}

static void test_warning_here_and_upstream_calls_for_evacuate_at_another_site() {
  assertUpstream(shallowCulvert());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_level_thresholds_are_percentages_of_bank_full_depth);
  RUN_TEST(test_alert_level_at_each_threshold_boundary_of_the_prototype_site);
  RUN_TEST(test_alert_level_at_each_threshold_boundary_of_another_site);
  RUN_TEST(test_rain_in_the_window_raises_watch_at_the_prototype_site);
  RUN_TEST(test_rain_in_the_window_raises_watch_at_another_site);
  RUN_TEST(test_warning_here_and_upstream_calls_for_evacuate_at_the_prototype_site);
  RUN_TEST(test_warning_here_and_upstream_calls_for_evacuate_at_another_site);
  return UNITY_END();
}
