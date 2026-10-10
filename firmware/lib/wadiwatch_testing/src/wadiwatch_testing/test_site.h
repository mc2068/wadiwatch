// A whole site on a PC: the device cores wired to fake ports and joined by an in-memory radio.
#pragma once

#include <cstdint>

#include "wadiwatch_testing/fakes.h"
#include "wadiwatch/siren_node.h"
#include "wadiwatch/site_config.h"
#include "wadiwatch/wadi_node.h"

namespace wadiwatch {
namespace testing {

struct TestSite {
  SiteConfig config = prototypeSite();
  FakeClock clock;
  Air air;
  FakeSleepControl wadiNodeSleep{clock};
  FakeLevelSensor levelSensor{config.wadiNode.mountingHeightCm};
  FakeRainGauge rainGauge{config.wadiNode.rain.mmPerTip, wadiNodeSleep};
  FakeNodeHealth wadiNodeHealth;
  FakeSirenOutput siren;
  WadiNode wadiNode{config.wadiNode, clock,          wadiNodeSleep, levelSensor,
                    rainGauge,       wadiNodeHealth, air.join()};
  SirenNode sirenNode{air.join(), siren};

  // Advances simulated time one second at a time, ticking every device that is awake.
  // The wadi node sleeps between pieces of work; a rain-gauge tip wakes it early.
  void runMs(uint64_t durationMs) {
    const uint64_t stepMs = 1000;
    for (uint64_t elapsed = 0; elapsed < durationMs; elapsed += stepMs) {
      clock.advanceMs(stepMs);
      if (!wadiNodeSleep.isAsleep()) wadiNode.tick();
      sirenNode.tick();
    }
  }

  void runMinutes(uint64_t minutes) { runMs(minutes * 60 * 1000); }
};

}  // namespace testing
}  // namespace wadiwatch
