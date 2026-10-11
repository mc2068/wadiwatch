// A node that the scenario plays itself: it signs packets in one node's name, with
// whatever key it is given, and hands back the frames for the scenario to put on the
// air (`site.air.inject`). Its counter goes up with every packet, as a real node's does.
//
// Use it for a wadi node whose transmissions the scenario times itself, and for a
// forger: someone without the site key, or someone outside the site's node list.
// The genuine all clear comes from the site's own gateway (`site.issueAllClear()`).
// To play a node the site does have, take the real one off the air first
// (`site.unplugWadiNode()`): two transmitters counting separately under one identifier
// would have their packets refused as replays of each other's.
#pragma once

#include <cstdint>

#include "wadiwatch/packet.h"
#include "wadiwatch/site_config.h"
#include "wadiwatch_testing/fakes.h"

namespace wadiwatch {
namespace testing {

class ScriptedNode {
 public:
  ScriptedNode(const SiteAuth& auth, NodeId id) : codec_(auth, id, storage_) {}

  Frame alert(AlertLevel alertLevel) {
    Packet packet = Packet();
    packet.kind = PacketKind::Alert;
    packet.alert.alertLevel = alertLevel;
    return sign(packet);
  }

  // An all clear for the event at one wadi node, as a forger would send it.
  Frame allClear(NodeId wadiNode) {
    Packet packet = Packet();
    packet.kind = PacketKind::AllClear;
    packet.allClear.wadiNode = wadiNode;
    return sign(packet);
  }

 private:
  Frame sign(Packet& packet) {
    uint8_t buffer[kMaxPacketSize];
    const size_t length = codec_.encode(packet, buffer, sizeof(buffer));
    return Frame(buffer, buffer + length);
  }

  FakeStorage storage_;
  PacketCodec codec_;
};

}  // namespace testing
}  // namespace wadiwatch
