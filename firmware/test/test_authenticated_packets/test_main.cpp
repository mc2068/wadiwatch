// Scenario tests for packet authentication: forged, foreign and replayed alerts
// must never sound the siren, and genuine ones must keep working across restarts.
#include <unity.h>

#include "wadiwatch/alert_engine.h"
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
using wadiwatch::testing::SirenSound;
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

// The wadi floods to its Warning threshold and the wadi node measures it:
// one Warning alert goes on the air.
static void floodWadi(TestSite& site) {
  const wadiwatch::WadiNodeConfig& config = site.config.wadiNode;
  site.levelSensor.setWaterLevelCm(
      wadiwatch::levelThresholdCm(config.alertThresholds, AlertLevel::Warning));
  const size_t readsBefore = site.levelSensor.readCount();
  for (uint64_t waited = 0; site.levelSensor.readCount() == readsBefore;
       waited += TestSite::kStepMs) {
    TEST_ASSERT_TRUE_MESSAGE(waited < config.mode.dryMeasureIntervalMs,
                             "the wadi node never measured the level");
    site.runMs(TestSite::kStepMs);
  }
}

// A minute goes by, then a bystander listens: at Warning the siren comes and
// goes, so it is listened to over a whole pattern period.
static bool sirenSounds(TestSite& site) {
  site.runMinutes(1);
  const bool sounds = site.listenToSiren() != SirenSound::Silent;
  // The strobe never disagrees with the siren about whether there is an alert.
  TEST_ASSERT_EQUAL(sounds, site.siren.isStrobeOn());
  return sounds;
}

// Control for the scenarios below: the same alert, signed with the site key
// in the name of the site's wadi node, does sound the siren.
static void test_alert_signed_with_the_site_key_by_a_site_node_sounds_the_siren() {
  TestSite site;

  site.air.inject(signedAlert(site.config.auth, site.config.wadiNode.id));

  TEST_ASSERT_TRUE(sirenSounds(site));
}

static void test_alert_signed_with_the_wrong_key_does_not_sound_the_siren() {
  TestSite site;
  SiteAuth forgerAuth = site.config.auth;
  forgerAuth.key[0] ^= 0x01;

  site.air.inject(signedAlert(forgerAuth, site.config.wadiNode.id));

  TEST_ASSERT_FALSE(sirenSounds(site));
}

static void test_genuine_alert_altered_in_transit_does_not_sound_the_siren() {
  TestSite source;
  floodWadi(source);
  TEST_ASSERT_TRUE(sirenSounds(source));
  // The node goes on sending readings; the frame to tamper with is its alert.
  const Frame genuine = source.framesOnAir(PacketKind::Alert).back();

  // The same site with a dry wadi: its siren node has not heard this alert yet.
  TestSite site;
  for (size_t i = 0; i < genuine.size(); ++i) {
    Frame altered = genuine;
    altered[i] ^= 0x01;
    site.air.inject(altered);
    TEST_ASSERT_FALSE_MESSAGE(sirenSounds(site), "an altered frame sounded the siren");
  }

  // The untouched frame is still good: the altered copies did not use up its counter.
  site.air.inject(genuine);
  TEST_ASSERT_TRUE(sirenSounds(site));
}

static void test_signed_alert_from_a_node_outside_the_site_does_not_sound_the_siren() {
  TestSite site;

  site.air.inject(signedAlert(site.config.auth, site.nodeIdOutsideSite()));

  TEST_ASSERT_FALSE(sirenSounds(site));
}

static void test_replayed_alert_does_not_sound_the_siren() {
  TestSite site;
  floodWadi(site);
  TEST_ASSERT_TRUE(sirenSounds(site));
  const Frame recorded = site.framesOnAir(PacketKind::Alert).back();

  // The siren node is power-cycled, which silences it, and remembers what it accepted.
  site.restartSirenNode();
  TEST_ASSERT_FALSE(sirenSounds(site));

  site.air.inject(recorded);

  TEST_ASSERT_FALSE(sirenSounds(site));
}

static void test_wadi_node_restarted_during_a_flood_is_still_believed() {
  TestSite site;
  floodWadi(site);
  TEST_ASSERT_TRUE(sirenSounds(site));
  site.restartSirenNode();
  TEST_ASSERT_FALSE(sirenSounds(site));

  // The wadi node restarts, finds the wadi still in flood and raises its alert again.
  // The siren node accepts it only if the counter carried on from before the restart.
  site.restartWadiNode();
  floodWadi(site);

  TEST_ASSERT_TRUE(sirenSounds(site));
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
