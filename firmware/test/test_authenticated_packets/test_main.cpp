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
  const Frame genuine = source.air.transmitted().back();

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
  // A genuine alert, recorded off the air during a flood.
  TestSite source;
  floodWadi(source);
  const Frame recorded = source.air.transmitted().back();

  // The same site with a dry wadi, so that no wadi node is sending its alert again:
  // the siren node hears the recorded alert once, as it did during the flood.
  TestSite site;
  site.air.inject(recorded);
  TEST_ASSERT_TRUE(sirenSounds(site));

  // The siren node is power-cycled, which silences it, and remembers what it accepted.
  site.restartSirenNode();
  TEST_ASSERT_FALSE(sirenSounds(site));

  site.air.inject(recorded);

  TEST_ASSERT_FALSE(sirenSounds(site));
}

static void test_wadi_node_restarted_during_a_flood_is_still_believed() {
  TestSite site;
  floodWadi(site);
  // The flood lasts: the wadi node sends its alert again several times and the siren
  // node accepts each one, so the counters are well past their first values.
  const uint64_t resendsBeforeTheRestart = 10;
  site.runMs(resendsBeforeTheRestart * site.config.wadiNode.alertResendIntervalMs);
  TEST_ASSERT_TRUE(sirenSounds(site));

  // Both nodes lose power together. The siren node comes back silent and remembers
  // the last counter it accepted; the wadi node finds the wadi still in flood and
  // raises its alert again.
  site.restartSirenNode();
  site.restartWadiNode();
  floodWadi(site);

  // The siren node accepts that alert only if the wadi node's counter carried on from
  // before the restart. Counting from one again, the wadi node would be ignored
  // until its re-sends had caught up, which takes longer than this.
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
