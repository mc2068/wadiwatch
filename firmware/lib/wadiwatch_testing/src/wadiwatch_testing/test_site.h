// A whole site on a PC: the device cores wired to fake ports and joined by an in-memory radio.
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "wadiwatch_testing/fakes.h"
#include "wadiwatch_testing/scripted_node.h"
#include "wadiwatch/gateway.h"
#include "wadiwatch/packet.h"
#include "wadiwatch/recipients.h"
#include "wadiwatch/siren_node.h"
#include "wadiwatch/site_config.h"
#include "wadiwatch/wadi_node.h"

namespace wadiwatch {
namespace testing {

// What a bystander hears from the siren over one period of the intermittent pattern.
enum class SirenSound { Silent, Intermittent, Continuous };

// A wadi node with its own fake hardware around it.
class WadiNodeRig {
  // Declared first: the members below are built from them. They belong to the TestSite.
  const WadiNodeConfig& config_;
  const SiteAuth& auth_;
  FakeClock& clock_;

 public:
  WadiNodeRig(const WadiNodeConfig& config, const SiteAuth& auth, FakeClock& clock, Air& air)
      : config_(config), auth_(auth), clock_(clock), radio(air.join(&sleep)) {}

  FakeSleepControl sleep{clock_};
  FakeLevelSensor levelSensor{config_.mountingHeightCm, config_.levelSensor.referenceTemperatureC};
  FakeTemperatureSensor temperatureSensor;
  FakeRainGauge rainGauge{config_.rain.mmPerTip, sleep};
  FakeNodeHealth health;
  FakeRandom random;
  FakeStorage storage;
  InMemoryRadio& radio;
  std::unique_ptr<WadiNode> node = makeNode();

  // Power-cycles the node: see TestSite::restartWadiNode().
  void restart() {
    sleep.wake();
    node = makeNode();
  }

  // Takes the node off the air: see TestSite::unplugWadiNode().
  void unplug() { node.reset(); }

 private:
  std::unique_ptr<WadiNode> makeNode() {
    return std::unique_ptr<WadiNode>(new WadiNode(config_, auth_, clock_, sleep, levelSensor,
                                                  rainGauge, health, radio, storage, random,
                                                  &temperatureSensor));
  }
};

struct TestSite {
  // Simulated time advances in steps of this size.
  static constexpr uint64_t kStepMs = 1000;

  SiteConfig config;
  FakeClock clock;
  Air air;
  // The first wadi node, `config.wadiNode`: the upstream one unless a scenario
  // configures the site otherwise.
  FakeSleepControl wadiNodeSleep{clock};
  FakeLevelSensor levelSensor{config.wadiNode.mountingHeightCm,
                              config.wadiNode.levelSensor.referenceTemperatureC};
  FakeTemperatureSensor temperatureSensor;
  FakeRainGauge rainGauge{config.wadiNode.rain.mmPerTip, wadiNodeSleep};
  FakeNodeHealth wadiNodeHealth;
  FakeRandom wadiNodeRandom;
  FakeSirenOutput siren;
  FakeNodeHealth sirenNodeHealth;
  FakeButton sirenButton;
  FakeStorage wadiNodeStorage;
  FakeStorage sirenNodeStorage;
  FakeStorage gatewayStorage;
  InMemoryRadio& wadiNodeRadio = air.join(&wadiNodeSleep);
  InMemoryRadio& sirenNodeRadio = air.join();
  InMemoryRadio& gatewayRadio = air.join();
  // The recipient list entered on the gateway.
  RecipientList recipients;
  FakeGsmModem modem;
  // The gateway's way to the backend, the time of day it dates messages with,
  // and the storage its messages wait in.
  FakeWallClock wallClock{clock};
  FakeUplink uplink;
  FakeSlotStorage gatewayMessageStorage{kMaxStoredMessages};
  std::unique_ptr<WadiNode> wadiNode = makeWadiNode();
  // The second wadi node, `config.downstreamWadiNode`, with its own sensors:
  // `downstream.levelSensor`, `downstream.rainGauge`, `downstream.node` and so on.
  WadiNodeRig downstream{config.downstreamWadiNode, config.auth, clock, air};
  std::unique_ptr<SirenNode> sirenNode = makeSirenNode();
  std::unique_ptr<Gateway> gateway = makeGateway();

  TestSite() : TestSite(prototypeSite()) {}
  explicit TestSite(const SiteConfig& siteConfig) : TestSite(siteConfig, prototypeRecipients()) {}
  TestSite(const SiteConfig& siteConfig, const RecipientList& recipientList)
      : config(siteConfig), recipients(recipientList) {}

  // Advances simulated time one step at a time, ticking every device that is awake.
  // A wadi node sleeps between pieces of work; a tip of its rain gauge wakes it
  // early, and so does a frame arriving while its receiver is on.
  void runMs(uint64_t durationMs) {
    for (uint64_t elapsed = 0; elapsed < durationMs; elapsed += kStepMs) {
      clock.advanceMs(kStepMs);
      if (wadiNode && !wadiNodeSleep.isAsleep()) wadiNode->tick();
      if (downstream.node && !downstream.sleep.isAsleep()) downstream.node->tick();
      sirenNode->tick();
      if (gatewayPluggedIn_) gateway->tick();
    }
  }

  // Takes the gateway out of the site, as if it had lost power: from now on it
  // hears nothing and sends nothing.
  void unplugGateway() { gatewayPluggedIn_ = false; }

  void runMinutes(uint64_t minutes) { runMs(minutes * 60 * 1000); }

  // Every packet the site's devices have put on the air, oldest first, as a
  // listener holding the site key accepts them. This is the one place in the
  // tests that reads frames, and it reads them the way a device does: through
  // the codec. A frame the codec refuses is not listed, and a transmission
  // repeated byte for byte is listed once.
  std::vector<Packet> packetsOnAir() const {
    std::vector<Packet> packets;
    for (const Heard& heard : heardOnAir()) packets.push_back(heard.packet);
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

  // The packets one node has sent, among packetsOnAir(). Every device of the site
  // transmits (heartbeats, acknowledgements, test packets), so a scenario about
  // one device's traffic asks for that device's packets.
  std::vector<Packet> packetsOnAirFrom(NodeId sender) const {
    std::vector<Packet> packets;
    for (const Packet& packet : packetsOnAir()) {
      if (packet.sender == sender) packets.push_back(packet);
    }
    return packets;
  }

  // The packets of one kind among packetsOnAirFrom(sender).
  std::vector<Packet> packetsOnAir(PacketKind kind, NodeId sender) const {
    std::vector<Packet> packets;
    for (const Packet& packet : packetsOnAirFrom(sender)) {
      if (packet.kind == kind) packets.push_back(packet);
    }
    return packets;
  }

  // The frames that carried the packets of one kind, oldest first, each as it was
  // first transmitted: what a recorder beside the site would have on tape. For
  // scenarios that play a frame back or count its transmissions; to read what a
  // frame says, use packetsOnAir().
  std::vector<Frame> framesOnAir(PacketKind kind) const {
    std::vector<Frame> frames;
    for (const Heard& heard : heardOnAir()) {
      if (heard.packet.kind == kind) frames.push_back(heard.frame);
    }
    return frames;
  }

  // How many times the site's devices have transmitted exactly this frame,
  // lost transmissions included.
  size_t timesTransmitted(const Frame& frame) const {
    return static_cast<size_t>(
        std::count(air.transmitted().begin(), air.transmitted().end(), frame));
  }

  // The alert level carried by the most recent alert packet put on the air,
  // or None when no alert has been transmitted.
  AlertLevel latestAlertOnAir() const {
    const std::vector<Packet> alerts = packetsOnAir(PacketKind::Alert);
    return alerts.empty() ? AlertLevel::None : alerts.back().alert.alertLevel;
  }

  // The same for one wadi node: the alert level it last announced.
  AlertLevel latestAlertOnAir(NodeId sender) const {
    const std::vector<Packet> alerts = packetsOnAir(PacketKind::Alert, sender);
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
  // After unplugWadiNode() this is the power coming back.
  // These two act on the first wadi node; `downstream.restart()` and
  // `downstream.unplug()` do the same for the second.
  void restartWadiNode() {
    wadiNodeSleep.wake();
    wadiNode = makeWadiNode();
  }

  // The wadi node loses power, or is carried off by the flood: from now on it
  // measures nothing and transmits nothing.
  void unplugWadiNode() { wadiNode.reset(); }

  void restartSirenNode() {
    siren = FakeSirenOutput();  // outputs are off after a reset
    sirenNode = makeSirenNode();
  }

  // The gateway forgets the events in progress; the modem, the uplink and
  // what they have already sent are untouched.
  void restartGateway() { gateway = makeGateway(); }

 private:
  struct Heard {
    Frame frame;
    Packet packet;
  };

  std::vector<Heard> heardOnAir() const {
    FakeStorage listenerStorage;
    PacketCodec listener(config.auth, nodeIdOutsideSite(), listenerStorage);
    std::vector<Heard> heard;
    for (const Frame& frame : air.transmitted()) {
      Packet packet;
      if (listener.decode(frame.data(), frame.size(), packet)) heard.push_back({frame, packet});
    }
    return heard;
  }

  std::unique_ptr<WadiNode> makeWadiNode() {
    return std::unique_ptr<WadiNode>(
        new WadiNode(config.wadiNode, config.auth, clock, wadiNodeSleep, levelSensor, rainGauge,
                     wadiNodeHealth, wadiNodeRadio, wadiNodeStorage, wadiNodeRandom,
                     &temperatureSensor));
  }

  std::unique_ptr<SirenNode> makeSirenNode() {
    return std::unique_ptr<SirenNode>(
        new SirenNode(config.sirenNode, config.auth, clock, sirenNodeRadio, siren,
                      sirenNodeStorage, sirenNodeHealth, sirenButton));
  }

  std::unique_ptr<Gateway> makeGateway() {
    return std::unique_ptr<Gateway>(new Gateway(config.gateway, config.auth, recipients, clock,
                                                gatewayRadio, modem, gatewayStorage, wallClock,
                                                uplink, gatewayMessageStorage));
  }

  bool gatewayPluggedIn_ = true;
};

}  // namespace testing
}  // namespace wadiwatch
