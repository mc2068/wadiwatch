// Siren node core: answers authenticated alerts heard over the radio with the siren and the strobe.
// Warning sounds siren pattern 1 (intermittent), Evacuate a continuous siren;
// the strobe is on in both cases. Watch produces no output.
//
// An accepted alert is acknowledged over the radio and latched: it stays in force,
// across restarts, until an accepted all clear for the wadi node that raised it. A
// latched Warning or Evacuate that hears no alert from its wadi node for the
// configured timeout falls silent, which the heartbeat reports, and sounds again
// when the wadi node is heard again.
//
// The button under the cover starts a test (siren and strobe on, reported as a test
// packet) and silences one. While an alert is sounding the button does nothing.
#pragma once

#include <cstddef>
#include <cstdint>

#include "wadiwatch/packet.h"
#include "wadiwatch/ports.h"
#include "wadiwatch/site_config.h"

namespace wadiwatch {

class SirenNode {
 public:
  SirenNode(const SirenNodeConfig& config,
            const SiteAuth& auth,
            Clock& clock,
            Radio& radio,
            SirenOutput& output,
            PersistentStorage& storage,
            NodeHealth& health,
            Button& button);

  // Call repeatedly, at least as often as the shortest phase of the intermittent
  // pattern; handles every frame received since the last call and keeps the
  // siren pattern in time.
  void tick();

 private:
  // What one node of the site has this siren node doing. Alerts are latched per
  // wadi node because an all clear ends the event at one wadi node only.
  struct LatchedAlert {
    NodeId wadiNode;
    // Highest alert level heard from that node since its last all clear.
    AlertLevel alertLevel;
    // When an alert was last accepted from that node.
    uint64_t refreshedAtMs;
    // Set while a Warning or Evacuate has gone a whole timeout without a refresh.
    bool refreshLost;
  };

  void start(uint64_t nowMs);
  void handleReceivedFrames(uint64_t nowMs);
  void handleAlert(const Packet& alert, uint64_t nowMs);
  void handleAllClear(const Packet& allClear);
  void handleButton(uint64_t nowMs);
  void checkAlertRefresh(uint64_t nowMs);
  void driveOutput(uint64_t nowMs);
  void sendHeartbeat();
  void send(Packet& packet);
  LatchedAlert* latchFor(NodeId wadiNode);
  void storeLatch(const LatchedAlert& latch);
  AlertLevel soundingAlertLevel() const;
  bool refreshLost() const;

  SirenNodeConfig config_;
  Clock& clock_;
  Radio& radio_;
  SirenOutput& output_;
  PersistentStorage& storage_;
  NodeHealth& health_;
  Button& button_;
  PacketCodec codec_;
  // One slot for every other node of the site.
  LatchedAlert latches_[kMaxSiteNodes];
  size_t latchCount_ = 0;
  bool started_ = false;
  uint64_t nextHeartbeatAtMs_ = 0;
  bool testRunning_ = false;
  uint64_t testStartedAtMs_ = 0;
  // The alert level the output was last driven for.
  AlertLevel outputAlertLevel_ = AlertLevel::None;
  uint64_t intermittentStartedAtMs_ = 0;
  bool sirenOn_ = false;
  bool strobeOn_ = false;
};

}  // namespace wadiwatch
