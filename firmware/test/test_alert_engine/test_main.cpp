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

// A calm wadi apart from its level: no rise, no rain, nothing heard from upstream.
AlertInputs levelOnly(float levelCm) {
  const AlertInputs inputs = {levelCm, 0.0f, 0.0f, AlertLevel::None};
  return inputs;
}

// What the rain calls for, on its own and together with the level.
struct RainRow {
  const char* description;
  float levelCm;
  float rainInWindowMm;
  AlertLevel expected;
};

// No rise and nothing heard from upstream.
AlertInputs levelAndRain(float levelCm, float rainInWindowMm) {
  const AlertInputs inputs = {levelCm, 0.0f, rainInWindowMm, AlertLevel::None};
  return inputs;
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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_level_thresholds_are_percentages_of_bank_full_depth);
  RUN_TEST(test_alert_level_at_each_threshold_boundary_of_the_prototype_site);
  RUN_TEST(test_alert_level_at_each_threshold_boundary_of_another_site);
  RUN_TEST(test_rain_in_the_window_raises_watch_at_the_prototype_site);
  RUN_TEST(test_rain_in_the_window_raises_watch_at_another_site);
  return UNITY_END();
}
