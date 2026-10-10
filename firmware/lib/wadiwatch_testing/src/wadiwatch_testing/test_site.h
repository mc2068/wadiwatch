// A whole site on a PC: the device cores wired to fake ports and joined by an in-memory radio.
#pragma once

#include <cstdint>

#include "wadiwatch_testing/fakes.h"
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

struct TestSite {
  // Simulated time advances in steps of this size.
  static constexpr uint64_t kStepMs = 1000;

  SiteConfig config;
  FakeClock clock;
  Air air;
  FakeLevelSensor levelSensor{config.wadiNode.mountingHeightCm};
  FakeSirenOutput siren;
  WadiNode wadiNode{config.wadiNode, clock, levelSensor, air.join()};
  SirenNode sirenNode{config.sirenNode, clock, air.join(), siren};
  // The recipient list entered on the gateway.
  RecipientList recipients;
  FakeGsmModem modem;
  Gateway gateway{config.gateway, recipients, clock, air.join(), modem};

  TestSite() : TestSite(prototypeSite()) {}
  explicit TestSite(const SiteConfig& siteConfig) : TestSite(siteConfig, prototypeRecipients()) {}
  TestSite(const SiteConfig& siteConfig, const RecipientList& recipientList)
      : config(siteConfig), recipients(recipientList) {}

  // Advances simulated time one step at a time, ticking every device.
  void runMs(uint64_t durationMs) {
    for (uint64_t elapsed = 0; elapsed < durationMs; elapsed += kStepMs) {
      clock.advanceMs(kStepMs);
      wadiNode.tick();
      sirenNode.tick();
      if (gatewayPluggedIn_) gateway.tick();
    }
  }

  // Takes the gateway out of the site, as if it had lost power: from now on it
  // hears nothing and sends nothing.
  void unplugGateway() { gatewayPluggedIn_ = false; }

  void runMinutes(uint64_t minutes) { runMs(minutes * 60 * 1000); }

  // The alert level carried by the most recent alert packet put on the air,
  // or None when no alert has been transmitted.
  AlertLevel latestAlertOnAir() const {
    const std::vector<Frame>& frames = air.transmitted();
    for (size_t i = frames.size(); i > 0; --i) {
      const Frame& frame = frames[i - 1];
      Packet packet;
      if (!decodePacket(frame.data(), frame.size(), packet)) continue;
      if (packet.kind == PacketKind::Alert) return packet.alertLevel;
    }
    return AlertLevel::None;
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

 private:
  bool gatewayPluggedIn_ = true;
};

}  // namespace testing
}  // namespace wadiwatch
