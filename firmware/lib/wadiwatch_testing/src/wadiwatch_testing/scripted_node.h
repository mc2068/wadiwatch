// A node that the scenario plays itself: it signs packets in one node's name, with
// whatever key it is given, and hands back the frames for the scenario to put on the
// air (`site.air.inject`). Its counter goes up with every packet, as a real node's does.
//
// Use it for a transmission no core of the site makes yet (the all clear), for a
// wadi node whose transmissions the scenario times itself, and for a forger. To
// play a node the site does have, take the real one off the air first
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

  // The all clear for the event at one wadi node.
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

// Puts one more node on a site's list and returns its identifier.
inline NodeId addNode(SiteConfig& config) {
  NodeId highest = 0;
  for (size_t i = 0; i < config.auth.nodeCount; ++i) {
    if (config.auth.nodes[i] > highest) highest = config.auth.nodes[i];
  }
  const NodeId added = static_cast<NodeId>(highest + 1);
  config.auth.nodes[config.auth.nodeCount++] = added;
  return added;
}

// Stands in for the one thing the real gateway cannot do yet: send an all clear
// (ticket #14). Use it only in scenarios that need an all clear on the air.
//
// It is a transmitter of its own, with its own identifier added to the site's node
// list and its own packet counter. It does not borrow the identifier of the site's
// gateway (`config.gateway.id`): the real gateway core stays in the site, hears
// everything the scenario does and keeps its own counters, so nothing here has to
// change when that core starts to transmit. Build the TestSite from `config`:
//
//   ScriptedGateway gateway;
//   TestSite site(gateway.config);
//   site.air.inject(gateway.allClear(site.config.wadiNode.id));
//
// When the real gateway sends the all clear, scenarios should make it do so and
// this struct can go.
struct ScriptedGateway {
  explicit ScriptedGateway(const SiteConfig& base = prototypeSite())
      : config(base), id(addNode(config)), node(config.auth, id) {}

  Frame allClear(NodeId wadiNode) { return node.allClear(wadiNode); }

  SiteConfig config;
  NodeId id;
  ScriptedNode node;
};

}  // namespace testing
}  // namespace wadiwatch
