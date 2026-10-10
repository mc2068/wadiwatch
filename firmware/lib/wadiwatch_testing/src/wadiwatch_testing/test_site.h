// A whole site on a PC: the device cores wired to fake ports and joined by an in-memory radio.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "wadiwatch_testing/fakes.h"
#include "wadiwatch/packet.h"
#include "wadiwatch/siren_node.h"
#include "wadiwatch/site_config.h"
#include "wadiwatch/wadi_node.h"

namespace wadiwatch {
namespace testing {

// What a bystander hears from the siren over one period of the intermittent pattern.
enum class SirenSound { Silent, Intermittent, Continuous };

struct TestSite {
  // Simulated time advances in steps of this size.
  static constexpr uint64_t kStepMs = 1000;

  SiteConfig config;
  FakeClock clock;
  Air air;
  FakeSleepControl wadiNodeSleep{clock};
  FakeLevelSensor levelSensor{config.wadiNode.mountingHeightCm};
  FakeRainGauge rainGauge{config.wadiNode.rain.mmPerTip, wadiNodeSleep};
  FakeNodeHealth wadiNodeHealth;
  FakeSirenOutput siren;
  FakeStorage wadiNodeStorage;
  FakeStorage sirenNodeStorage;
  InMemoryRadio& wadiNodeRadio = air.join();
  InMemoryRadio& sirenNodeRadio = air.join();
  std::unique_ptr<WadiNode> wadiNode = makeWadiNode();
  std::unique_ptr<SirenNode> sirenNode = makeSirenNode();

  TestSite() : TestSite(prototypeSite()) {}
  explicit TestSite(const SiteConfig& siteConfig) : config(siteConfig) {}

  // Advances simulated time one step at a time, ticking every device that is awake.
  // The wadi node sleeps between pieces of work; a rain-gauge tip wakes it early.
  void runMs(uint64_t durationMs) {
    for (uint64_t elapsed = 0; elapsed < durationMs; elapsed += kStepMs) {
      clock.advanceMs(kStepMs);
      if (!wadiNodeSleep.isAsleep()) wadiNode->tick();
      sirenNode->tick();
    }
  }

  void runMinutes(uint64_t minutes) { runMs(minutes * 60 * 1000); }

  // Every packet the site's devices have put on the air, oldest first, as a
  // listener holding the site key accepts them. This is the one place in the
  // tests that reads frames, and it reads them the way a device does: through
  // the codec. A frame the codec refuses is not listed, and a transmission
  // repeated byte for byte is listed once.
  std::vector<Packet> packetsOnAir() const {
    FakeStorage listenerStorage;
    PacketCodec listener(config.auth, nodeIdOutsideSite(), listenerStorage);
    std::vector<Packet> packets;
    for (const Frame& frame : air.transmitted()) {
      Packet packet;
      if (listener.decode(frame.data(), frame.size(), packet)) packets.push_back(packet);
    }
    return packets;
  }

  // The packets of one kind among packetsOnAir().
  std::vector<Packet> packetsOnAir(PacketKind kind) const {
    std::vector<Packet> packets;
    for (const Packet& packet : packetsOnAir()) {
      if (packet.kind == kind) packets.push_back(packet);
    }
    return packets;
  }

  // The alert level carried by the most recent alert packet put on the air,
  // or None when no alert has been transmitted.
  AlertLevel latestAlertOnAir() const {
    const std::vector<Packet> alerts = packetsOnAir(PacketKind::Alert);
    return alerts.empty() ? AlertLevel::None : alerts.back().alert.alertLevel;
  }

  // A node identifier that no node of the site has: what a listener, or a
  // transmitter that does not belong to the site, would call itself.
  NodeId nodeIdOutsideSite() const {
    NodeId highest = 0;
    for (size_t i = 0; i < config.auth.nodeCount; ++i) {
      if (config.auth.nodes[i] > highest) highest = config.auth.nodes[i];
    }
    return static_cast<NodeId>(highest + 1);
  }

  // Runs the site for one period of the intermittent pattern and names what was heard.
  // Needs the pattern's on and off times to be at least one step long.
  SirenSound listenToSiren() {
    const uint64_t periodMs =
        static_cast<uint64_t>(config.sirenNode.intermittentOnMs) + config.sirenNode.intermittentOffMs;
    bool heardSounding = false;
    bool heardSilence = false;
    for (uint64_t elapsed = 0; elapsed < periodMs; elapsed += kStepMs) {
      runMs(kStepMs);
      if (siren.isOn()) {
        heardSounding = true;
      } else {
        heardSilence = true;
      }
    }
    if (!heardSounding) return SirenSound::Silent;
    return heardSilence ? SirenSound::Intermittent : SirenSound::Continuous;
  }

  // Power-cycles a device: its core starts again from nothing, awake, with the
  // same persistent storage and the same hardware around it.
  void restartWadiNode() {
    wadiNodeSleep.wake();
    wadiNode = makeWadiNode();
  }

  void restartSirenNode() {
    siren = FakeSirenOutput();  // outputs are off after a reset
    sirenNode = makeSirenNode();
  }

 private:
  std::unique_ptr<WadiNode> makeWadiNode() {
    return std::unique_ptr<WadiNode>(
        new WadiNode(config.wadiNode, config.auth, clock, wadiNodeSleep, levelSensor, rainGauge,
                     wadiNodeHealth, wadiNodeRadio, wadiNodeStorage));
  }

  std::unique_ptr<SirenNode> makeSirenNode() {
    return std::unique_ptr<SirenNode>(new SirenNode(config.sirenNode, config.auth, clock,
                                                    sirenNodeRadio, siren, sirenNodeStorage));
  }
};

}  // namespace testing
}  // namespace wadiwatch
