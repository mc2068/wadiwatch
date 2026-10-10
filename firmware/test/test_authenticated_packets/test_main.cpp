// Scenario tests for packet authentication: forged, foreign and replayed alerts
// must never sound the siren, and genuine ones must keep working across restarts.
#include <unity.h>

#include "wadiwatch/packet.h"
#include "wadiwatch_testing/test_site.h"

using wadiwatch::AlertLevel;
using wadiwatch::NodeId;
using wadiwatch::Packet;
using wadiwatch::PacketCodec;
using wadiwatch::PacketKind;
using wadiwatch::SiteAuth;
using wadiwatch::testing::FakeStorage;
using wadiwatch::testing::Frame;
using wadiwatch::testing::TestSite;

void setUp() {}
void tearDown() {}

// What a third radio would transmit: a Warning alert claiming to come from `claimedSender`,
// signed with whatever key the transmitter holds. It is that transmitter's first packet.
static Frame signedAlert(const SiteAuth& auth, NodeId claimedSender) {
  FakeStorage storage;
  PacketCodec codec(auth, claimedSender, storage);
  Packet packet = Packet();
  packet.kind = PacketKind::Alert;
  packet.alert.alertLevel = AlertLevel::Warning;
  uint8_t buffer[wadiwatch::kMaxPacketSize];
  const size_t length = codec.encode(packet, buffer, sizeof(buffer));
  return Frame(buffer, buffer + length);
}

static NodeId nodeIdOutsideSite(const SiteAuth& auth) {
  NodeId highest = 0;
  for (size_t i = 0; i < auth.nodeCount; ++i) {
    if (auth.nodes[i] > highest) highest = auth.nodes[i];
  }
  return static_cast<NodeId>(highest + 1);
}

// The wadi floods and the wadi node takes its next measurement.
static void floodWadi(TestSite& site) {
  site.levelSensor.setWaterLevelCm(site.config.wadiNode.warningLevelCm);
  site.runMs(site.config.wadiNode.measureIntervalMs);
}

// Control for the scenarios below: the same alert, signed with the site key
// in the name of the site's wadi node, does sound the siren.
static void test_alert_signed_with_the_site_key_by_a_site_node_sounds_the_siren() {
  TestSite site;

  site.air.inject(signedAlert(site.config.auth, site.config.wadiNode.id));
  site.runMinutes(1);

  TEST_ASSERT_TRUE(site.siren.isOn());
}

static void test_alert_signed_with_the_wrong_key_does_not_sound_the_siren() {
  TestSite site;
  SiteAuth forgerAuth = site.config.auth;
  forgerAuth.key[0] ^= 0x01;

  site.air.inject(signedAlert(forgerAuth, site.config.wadiNode.id));
  site.runMinutes(1);

  TEST_ASSERT_FALSE(site.siren.isOn());
}

static void test_genuine_alert_altered_in_transit_does_not_sound_the_siren() {
  TestSite source;
  floodWadi(source);
  TEST_ASSERT_TRUE(source.siren.isOn());
  const Frame genuine = source.air.transmitted().back();

  // The same site with a dry wadi: its siren node has not heard this alert yet.
  TestSite site;
  for (size_t i = 0; i < genuine.size(); ++i) {
    Frame altered = genuine;
    altered[i] ^= 0x01;
    site.air.inject(altered);
    site.runMinutes(1);
    TEST_ASSERT_FALSE_MESSAGE(site.siren.isOn(), "an altered frame sounded the siren");
  }

  // The untouched frame is still good: the altered copies did not use up its counter.
  site.air.inject(genuine);
  site.runMinutes(1);
  TEST_ASSERT_TRUE(site.siren.isOn());
}

static void test_signed_alert_from_a_node_outside_the_site_does_not_sound_the_siren() {
  TestSite site;

  site.air.inject(signedAlert(site.config.auth, nodeIdOutsideSite(site.config.auth)));
  site.runMinutes(1);

  TEST_ASSERT_FALSE(site.siren.isOn());
}

static void test_replayed_alert_does_not_sound_the_siren() {
  TestSite site;
  floodWadi(site);
  TEST_ASSERT_TRUE(site.siren.isOn());
  const Frame recorded = site.air.transmitted().back();

  // The siren node is power-cycled, which silences it, and remembers what it accepted.
  site.restartSirenNode();
  TEST_ASSERT_FALSE(site.siren.isOn());

  site.air.inject(recorded);
  site.runMinutes(1);

  TEST_ASSERT_FALSE(site.siren.isOn());
}

static void test_wadi_node_restarted_during_a_flood_is_still_believed() {
  TestSite site;
  floodWadi(site);
  TEST_ASSERT_TRUE(site.siren.isOn());
  site.restartSirenNode();
  TEST_ASSERT_FALSE(site.siren.isOn());

  // The wadi node restarts, finds the wadi still in flood and raises its alert again.
  // The siren node accepts it only if the counter carried on from before the restart.
  site.restartWadiNode();
  site.runMs(site.config.wadiNode.measureIntervalMs);

  TEST_ASSERT_TRUE(site.siren.isOn());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_alert_signed_with_the_site_key_by_a_site_node_sounds_the_siren);
  RUN_TEST(test_alert_signed_with_the_wrong_key_does_not_sound_the_siren);
  RUN_TEST(test_genuine_alert_altered_in_transit_does_not_sound_the_siren);
  RUN_TEST(test_signed_alert_from_a_node_outside_the_site_does_not_sound_the_siren);
  RUN_TEST(test_replayed_alert_does_not_sound_the_siren);
  RUN_TEST(test_wadi_node_restarted_during_a_flood_is_still_believed);
  return UNITY_END();
}
