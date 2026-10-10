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
  FakeLevelSensor levelSensor{config.wadiNode.mountingHeightCm};
  FakeSirenOutput siren;
  WadiNode wadiNode{config.wadiNode, clock, levelSensor, air.join()};
  SirenNode sirenNode{air.join(), siren};

  // Advances simulated time one second at a time, ticking every device.
  void runMs(uint64_t durationMs) {
    const uint64_t stepMs = 1000;
    for (uint64_t elapsed = 0; elapsed < durationMs; elapsed += stepMs) {
      clock.advanceMs(stepMs);
      wadiNode.tick();
      sirenNode.tick();
    }
  }

  void runMinutes(uint64_t minutes) { runMs(minutes * 60 * 1000); }
};

}  // namespace testing
}  // namespace wadiwatch
