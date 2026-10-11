// A site where only the test speaks: for scenarios about what a device does with packets
// that no device sends yet, or that must arrive exactly as the scenario says.
#pragma once

#include <unity.h>

#include "wadiwatch/packet.h"
#include "wadiwatch/site_config.h"
#include "wadiwatch_testing/fakes.h"
#include "wadiwatch_testing/test_site.h"

namespace wadiwatch {
namespace testing {

// A radio in the test's hands. It signs packets as one node of the site, with the site
// key and that node's own counters, and transmits them: every device hears them, and
// TestSite::packetsOnAir() lists them like any other transmission.
struct TestRadio {
  TestRadio(TestSite& site, NodeId speaksAs)
      : id(speaksAs), codec(site.config.auth, speaksAs, storage), radio(site.air.join()) {}

  // Returns the packet as it went out, with its sender and counter filled in.
  Packet send(Packet packet) {
    uint8_t buffer[kMaxPacketSize];
    const size_t length = codec.encode(packet, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(kPacketSize, length, "the test radio could not encode");
    lastFrame = Frame(buffer, buffer + length);
    radio.send(buffer, length);
    return packet;
  }

  NodeId id;
  FakeStorage storage;
  PacketCodec codec;
  InMemoryRadio& radio;
  // The bytes of the last transmission, for a scenario that replays it.
  Frame lastFrame;
};

// The prototype site, except that its own wadi nodes and siren node call themselves by
// identifiers outside the node list. They run, but nobody believes a packet of theirs.
inline SiteConfig quietSiteConfig() {
  SiteConfig config = prototypeSite();
  NodeId highest = 0;
  for (size_t i = 0; i < config.auth.nodeCount; ++i) {
    if (config.auth.nodes[i] > highest) highest = config.auth.nodes[i];
  }
  config.wadiNode.id = static_cast<NodeId>(highest + 1);
  config.sirenNode.id = static_cast<NodeId>(highest + 2);
  config.downstreamWadiNode.id = static_cast<NodeId>(highest + 3);
  return config;
}

// The gateway (and whatever else listens) hears the site's wadi nodes and siren node
// through test radios only, so a scenario decides every packet that is accepted.
struct QuietSite : TestSite {
  QuietSite() : TestSite(quietSiteConfig()) {}

  TestRadio wadiNodeOnAir{*this, prototypeSite().wadiNode.id};
  TestRadio downstreamWadiNodeOnAir{*this, prototypeSite().downstreamWadiNode.id};
  TestRadio sirenNodeOnAir{*this, prototypeSite().sirenNode.id};

  // Transmits the packet and gives every device one turn, in which it hears it.
  // Returns the packet as it went out.
  Packet transmit(TestRadio& from, const Packet& packet) {
    const Packet sent = from.send(packet);
    runMs(kStepMs);
    return sent;
  }
};

}  // namespace testing
}  // namespace wadiwatch
