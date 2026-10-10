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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_level_thresholds_are_percentages_of_bank_full_depth);
  RUN_TEST(test_alert_level_at_each_threshold_boundary_of_the_prototype_site);
  RUN_TEST(test_alert_level_at_each_threshold_boundary_of_another_site);
  return UNITY_END();
}
