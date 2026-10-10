// A whole site on a PC: the device cores wired to fake ports and joined by an in-memory radio.
#pragma once

#include <cstdint>
#include <memory>

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
  FakeStorage wadiNodeStorage;
  FakeStorage sirenNodeStorage;
  InMemoryRadio& wadiNodeRadio = air.join();
  InMemoryRadio& sirenNodeRadio = air.join();
  std::unique_ptr<WadiNode> wadiNode = makeWadiNode();
  std::unique_ptr<SirenNode> sirenNode = makeSirenNode();

  // Advances simulated time one second at a time, ticking every device.
  void runMs(uint64_t durationMs) {
    const uint64_t stepMs = 1000;
    for (uint64_t elapsed = 0; elapsed < durationMs; elapsed += stepMs) {
      clock.advanceMs(stepMs);
      wadiNode->tick();
      sirenNode->tick();
    }
  }

  void runMinutes(uint64_t minutes) { runMs(minutes * 60 * 1000); }

  // Power-cycles a device: its core starts again from nothing, with the same
  // persistent storage and the same hardware around it.
  void restartWadiNode() { wadiNode = makeWadiNode(); }

  void restartSirenNode() {
    siren = FakeSirenOutput();  // outputs are off after a reset
    sirenNode = makeSirenNode();
  }

 private:
  std::unique_ptr<WadiNode> makeWadiNode() {
    return std::unique_ptr<WadiNode>(new WadiNode(config.wadiNode, config.auth, clock, levelSensor,
                                                  wadiNodeRadio, wadiNodeStorage));
  }

  std::unique_ptr<SirenNode> makeSirenNode() {
    return std::unique_ptr<SirenNode>(
        new SirenNode(config.sirenNode, config.auth, sirenNodeRadio, siren, sirenNodeStorage));
  }
};

}  // namespace testing
}  // namespace wadiwatch
