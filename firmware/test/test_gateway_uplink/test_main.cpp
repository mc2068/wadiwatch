// Scenario tests for the uplink: the gateway forwards every packet it accepts to the
// backend, as the messages of docs/gateway-contract.md, and keeps them through an uplink
// outage and a restart. They assert the messages the gateway hands to its uplink.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <unity.h>

#include "wadiwatch/alert_engine.h"
#include "wadiwatch/message_store.h"
#include "wadiwatch/packet.h"
#include "wadiwatch/site_config.h"
#include "wadiwatch/uplink_message.h"
#include "wadiwatch_testing/quiet_site.h"
#include "wadiwatch_testing/test_site.h"

using wadiwatch::AlertLevel;
using wadiwatch::GatewayConfig;
using wadiwatch::kMaxStoredMessages;
using wadiwatch::levelThresholdCm;
using wadiwatch::NodeId;
using wadiwatch::Packet;
using wadiwatch::PacketCodec;
using wadiwatch::PacketKind;
using wadiwatch::SiteAuth;
using wadiwatch::testing::FakeStorage;
using wadiwatch::testing::Frame;
using wadiwatch::testing::QuietSite;
using wadiwatch::testing::Sms;
using wadiwatch::testing::TestRadio;
using wadiwatch::testing::TestSite;

void setUp() {}
void tearDown() {}

namespace {

const uint64_t kOneHourMs = 60ull * 60 * 1000;
// With the uplink working, a message follows its packet within a few turns of the gateway.
const uint64_t kUplinkPromptMs = 5 * TestSite::kStepMs;
// Design target: an SMS follows its alert within a minute while GSM is available.
const uint64_t kSmsTargetMs = 60ull * 1000;

// --- Packets a test radio transmits -------------------------------------------------

Packet reading(uint16_t levelMm, int16_t rateOfRiseMmPerMin = 0) {
  Packet packet = Packet();
  packet.kind = PacketKind::Reading;
  packet.reading.levelMm = levelMm;
  packet.reading.rateOfRiseMmPerMin = rateOfRiseMmPerMin;
  return packet;
}

Packet alert(AlertLevel alertLevel) {
  Packet packet = Packet();
  packet.kind = PacketKind::Alert;
  packet.alert.alertLevel = alertLevel;
  return packet;
}

Packet acknowledgement(const Packet& acknowledged) {
  Packet packet = Packet();
  packet.kind = PacketKind::Acknowledgement;
  packet.acknowledgement.alertSender = acknowledged.sender;
  packet.acknowledgement.alertCounter = acknowledged.counter;
  packet.acknowledgement.alertLevel = acknowledged.alert.alertLevel;
  return packet;
}

Packet heartbeat(uint16_t batteryMillivolts) {
  Packet packet = Packet();
  packet.kind = PacketKind::Heartbeat;
  packet.heartbeat.batteryMillivolts = batteryMillivolts;
  return packet;
}

Packet allClear(NodeId wadiNode) {
  Packet packet = Packet();
  packet.kind = PacketKind::AllClear;
  packet.allClear.wadiNode = wadiNode;
  return packet;
}

Packet testPacket() {
  Packet packet = Packet();
  packet.kind = PacketKind::Test;
  return packet;
}

// --- Reading the messages the uplink took ---------------------------------------------

// The names the contract gives the packet kinds.
const char* contractName(PacketKind kind) {
  switch (kind) {
    case PacketKind::Reading:
      return "reading";
    case PacketKind::Alert:
      return "alert";
    case PacketKind::Acknowledgement:
      return "acknowledgement";
    case PacketKind::Heartbeat:
      return "heartbeat";
    case PacketKind::AllClear:
      return "all_clear";
    case PacketKind::Test:
      return "test";
  }
  return "?";
}

std::string number(uint64_t value) { return std::to_string(value); }

// The fields every message starts with, as the contract orders them, for the packet
// `sent` received by the gateway at `receivedAt`.
std::string messageStart(const Packet& sent, uint32_t receivedAt) {
  return std::string("{\"v\":1,\"kind\":\"") + contractName(sent.kind) + "\",\"node\":" +
         number(sent.sender) + ",\"counter\":" + number(sent.counter) +
         ",\"ts\":" + number(receivedAt);
}

// The value of a field that holds a whole number.
uint64_t numberField(const std::string& message, const char* name) {
  const std::string key = std::string("\"") + name + "\":";
  const size_t at = message.find(key);
  TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, name);
  return std::strtoull(message.c_str() + at + key.size(), nullptr, 10);
}

// The value of a field that holds a string.
std::string textField(const std::string& message, const char* name) {
  const std::string key = std::string("\"") + name + "\":\"";
  const size_t at = message.find(key);
  TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, name);
  const size_t start = at + key.size();
  return message.substr(start, message.find('"', start) - start);
}

// The value of a field that holds true or false.
bool flagField(const std::string& message, const char* name) {
  const std::string key = std::string("\"") + name + "\":";
  const size_t at = message.find(key);
  TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, name);
  return message.compare(at + key.size(), 4, "true") == 0;
}

uint32_t timeOf(const std::string& message) {
  return static_cast<uint32_t>(numberField(message, "ts"));
}

// Which packet a message stands for: kind, sender and counter.
std::string identityOf(const std::string& message) {
  return textField(message, "kind") + " from " + number(numberField(message, "node")) + " #" +
         number(numberField(message, "counter"));
}

std::string identityOf(const Packet& packet) {
  return std::string(contractName(packet.kind)) + " from " + number(packet.sender) + " #" +
         number(packet.counter);
}

template <typename Item>
std::string identitiesOf(const std::vector<Item>& items) {
  std::string all;
  for (const Item& item : items) all += identityOf(item) + "\n";
  return all;
}

// The uplink took one message for each of these packets, in this order, and nothing else.
void assertForwarded(const std::vector<Packet>& packets, const TestSite& site) {
  TEST_ASSERT_EQUAL_STRING(identitiesOf(packets).c_str(),
                           identitiesOf(site.uplink.published()).c_str());
}

// The message the uplink took for this packet.
std::string messageFor(const Packet& packet, const TestSite& site) {
  for (const std::string& message : site.uplink.published()) {
    if (identityOf(message) == identityOf(packet)) return message;
  }
  TEST_FAIL_MESSAGE(("not forwarded: " + identityOf(packet)).c_str());
  return std::string();
}

// --- Driving a site -------------------------------------------------------------------

float thresholdCm(const TestSite& site, AlertLevel alertLevel) {
  return levelThresholdCm(site.config.wadiNode.alertThresholds, alertLevel);
}

// A level that is clearly at `alertLevel` and clearly short of the next one up.
float levelWithin(const TestSite& site, AlertLevel alertLevel) {
  const float lowerCm = thresholdCm(site, alertLevel);
  const float upperCm = alertLevel == AlertLevel::Evacuate
                            ? site.config.wadiNode.alertThresholds.bankFullDepthCm
                            : thresholdCm(site, static_cast<AlertLevel>(
                                                    static_cast<uint8_t>(alertLevel) + 1));
  return (lowerCm + upperCm) / 2.0f;
}

// Raises the water until the wadi node has put `alertLevel` on the air.
void floodTo(TestSite& site, AlertLevel alertLevel) {
  site.levelSensor.setWaterLevelCm(levelWithin(site, alertLevel));
  for (uint64_t waited = 0; site.latestAlertOnAir() != alertLevel; waited += TestSite::kStepMs) {
    TEST_ASSERT_TRUE_MESSAGE(waited < kOneHourMs, "the wadi node never raised the alert level");
    site.runMs(TestSite::kStepMs);
  }
}

// Raises the water at both wadi nodes until each has announced what the site then comes
// to: Warning at the upstream wadi node, and Evacuate at the downstream one, which is at
// Warning itself and hears the upstream alert. From then on both send their alert again
// every minute.
void floodBothWadiNodes(TestSite& site) {
  const NodeId upstream = site.config.wadiNode.id;
  const NodeId downstream = site.config.downstreamWadiNode.id;
  site.levelSensor.setWaterLevelCm(levelWithin(site, AlertLevel::Warning));
  site.downstream.levelSensor.setWaterLevelCm(levelWithin(site, AlertLevel::Warning));
  for (uint64_t waited = 0; site.latestAlertOnAir(upstream) != AlertLevel::Warning ||
                            site.latestAlertOnAir(downstream) != AlertLevel::Evacuate;
       waited += TestSite::kStepMs) {
    TEST_ASSERT_TRUE_MESSAGE(waited < kOneHourMs, "the wadi nodes never raised their alerts");
    site.runMs(TestSite::kStepMs);
  }
}

// The alerts on the air that changed the alert level of the wadi node that sent them:
// every alert but the ones that state again the level of that node's alert before.
std::vector<Packet> changesOfAlertLevelOnAir(const TestSite& site) {
  std::vector<Packet> changes;
  std::vector<Packet> latest;  // one per wadi node: its latest alert so far
  for (const Packet& alertOnAir : site.packetsOnAir(PacketKind::Alert)) {
    bool heardBefore = false;
    for (Packet& previous : latest) {
      if (previous.sender != alertOnAir.sender) continue;
      heardBefore = true;
      if (previous.alert.alertLevel != alertOnAir.alert.alertLevel) changes.push_back(alertOnAir);
      previous = alertOnAir;
    }
    if (!heardBefore) {
      changes.push_back(alertOnAir);
      latest.push_back(alertOnAir);
    }
  }
  return changes;
}

// Every message the uplink took stands for a packet that was on the air, none stands
// for the same packet twice, and they left in the order the packets were sent.
void assertForwardedInOrderOnceEach(const TestSite& site) {
  const std::vector<Packet> onAir = site.packetsOnAir();
  size_t next = 0;
  for (const std::string& message : site.uplink.published()) {
    while (next < onAir.size() && identityOf(onAir[next]) != identityOf(message)) ++next;
    TEST_ASSERT_TRUE_MESSAGE(next < onAir.size(),
                             ("out of order, twice, or never sent: " + identityOf(message)).c_str());
    ++next;
  }
}

// Long enough for the gateway to have offered the uplink everything it holds, however
// full its store and however recently the uplink came back, and then until the air has
// been silent for a moment, so that nothing is still on its way.
void letTheUplinkCatchUp(TestSite& site) {
  site.runMs(site.config.gateway.uplinkRetryIntervalMs + kMaxStoredMessages * TestSite::kStepMs);
  size_t transmitted;
  do {
    transmitted = site.air.transmitted().size();
    site.runMs(3 * TestSite::kStepMs);
  } while (site.air.transmitted().size() != transmitted);
}

void endOutage(TestSite& site) {
  site.uplink.endOutage();
  letTheUplinkCatchUp(site);
}

// Time goes by in which the scenario sends nothing: this many of the gateway's uplink
// retry intervals, so that whatever the gateway would offer its uplink again, it has.
void quietSpell(TestSite& site, uint64_t retryIntervals) {
  site.runMs(retryIntervals * site.config.gateway.uplinkRetryIntervalMs);
}

// The battery voltage below which the site configuration calls this node's battery low.
uint16_t lowBatteryMillivolts(const TestSite& site, NodeId node) {
  const GatewayConfig& gateway = site.config.gateway;
  for (size_t i = 0; i < gateway.batteryCount; ++i) {
    if (gateway.batteries[i].node == node) return gateway.batteries[i].lowBatteryMillivolts;
  }
  TEST_FAIL_MESSAGE("the site configuration has no low-battery threshold for the node");
  return 0;
}

// A frame signed by whoever holds `auth`, claiming to come from `claimedSender`.
Frame signedReading(const SiteAuth& auth, NodeId claimedSender) {
  FakeStorage storage;
  PacketCodec codec(auth, claimedSender, storage);
  Packet packet = reading(500);
  uint8_t buffer[wadiwatch::kMaxPacketSize];
  const size_t length = codec.encode(packet, buffer, sizeof(buffer));
  return Frame(buffer, buffer + length);
}

// A gateway fitted with a store of only `slots` messages, cut off from the backend.
struct CutOffSite : QuietSite {
  explicit CutOffSite(size_t slots) {
    gatewayMessageStorage.setSlotCount(slots);
    uplink.startOutage();
  }

  Packet fromWadiNode(const Packet& packet) { return transmit(wadiNodeOnAir, packet); }
  Packet fromSirenNode(const Packet& packet) { return transmit(sirenNodeOnAir, packet); }
};

}  // namespace

// --- Every packet kind, in the documented message format ---------------------------------

static void test_a_reading_is_forwarded_with_everything_the_packet_carries() {
  QuietSite site;
  Packet packet = Packet();
  packet.kind = PacketKind::Reading;
  packet.reading.levelMm = 845;
  packet.reading.rateOfRiseMmPerMin = 32;
  packet.reading.rainfallTenthsMm = 124;
  packet.reading.alertLevel = AlertLevel::Watch;
  packet.reading.stormMode = true;

  const Packet sent = site.transmit(site.wadiNodeOnAir, packet);
  const uint32_t receivedAt = site.wallClock.unixSeconds();

  TEST_ASSERT_EQUAL_UINT(1, site.uplink.published().size());
  // Millimetres on the air, centimetres in the message.
  TEST_ASSERT_EQUAL_STRING(
      (messageStart(sent, receivedAt) +
       R"(,"level_cm":84.5,"rise_cm_min":3.2,"rain_mm":12.4,"storm_mode":true,"alert_level":"watch","level_repeated":false})")
          .c_str(),
      site.uplink.published()[0].c_str());
}

static void test_a_reading_of_a_falling_dry_wadi_is_forwarded_exactly() {
  QuietSite site;

  const Packet sent = site.transmit(site.wadiNodeOnAir, reading(7, -4));
  const uint32_t receivedAt = site.wallClock.unixSeconds();

  TEST_ASSERT_EQUAL_STRING(
      (messageStart(sent, receivedAt) +
       R"(,"level_cm":0.7,"rise_cm_min":-0.4,"rain_mm":0.0,"storm_mode":false,"alert_level":"none","level_repeated":false})")
          .c_str(),
      site.uplink.published().back().c_str());
}

static void test_the_largest_values_a_reading_can_carry_are_forwarded_exactly() {
  QuietSite site;
  Packet packet = reading(UINT16_MAX, INT16_MIN);
  packet.reading.rainfallTenthsMm = UINT16_MAX;
  packet.reading.alertLevel = AlertLevel::Evacuate;

  const Packet sent = site.transmit(site.wadiNodeOnAir, packet);
  const uint32_t receivedAt = site.wallClock.unixSeconds();

  TEST_ASSERT_EQUAL_STRING(
      (messageStart(sent, receivedAt) +
       R"(,"level_cm":6553.5,"rise_cm_min":-3276.8,"rain_mm":6553.5,"storm_mode":false,"alert_level":"evacuate","level_repeated":false})")
          .c_str(),
      site.uplink.published().back().c_str());
}

static void test_an_alert_is_forwarded_with_the_level_and_rate_of_rise_behind_it() {
  QuietSite site;
  Packet packet = alert(AlertLevel::Warning);
  packet.alert.levelMm = 1040;
  packet.alert.rateOfRiseMmPerMin = 51;

  const Packet sent = site.transmit(site.wadiNodeOnAir, packet);
  const uint32_t receivedAt = site.wallClock.unixSeconds();

  TEST_ASSERT_EQUAL_UINT(1, site.uplink.published().size());
  TEST_ASSERT_EQUAL_STRING(
      (messageStart(sent, receivedAt) +
       R"(,"alert_level":"warning","level_cm":104.0,"rise_cm_min":5.1,"refresh":false,"level_repeated":false})")
          .c_str(),
      site.uplink.published()[0].c_str());
}

static void test_an_alert_refresh_is_forwarded_as_one() {
  QuietSite site;
  site.transmit(site.wadiNodeOnAir, alert(AlertLevel::Warning));
  Packet again = alert(AlertLevel::Warning);
  again.alert.refresh = true;

  site.transmit(site.wadiNodeOnAir, again);

  TEST_ASSERT_EQUAL_UINT(2, site.uplink.published().size());
  TEST_ASSERT_FALSE(flagField(site.uplink.published()[0], "refresh"));
  TEST_ASSERT_TRUE(flagField(site.uplink.published()[1], "refresh"));
}

static void test_every_alert_level_is_forwarded_under_its_contract_name() {
  QuietSite site;
  const AlertLevel levels[] = {AlertLevel::None, AlertLevel::Watch, AlertLevel::Warning,
                               AlertLevel::Evacuate};
  const char* const names[] = {"none", "watch", "warning", "evacuate"};

  for (const AlertLevel level : levels) site.transmit(site.wadiNodeOnAir, alert(level));

  TEST_ASSERT_EQUAL_UINT(4, site.uplink.published().size());
  for (size_t i = 0; i < 4; ++i) {
    TEST_ASSERT_EQUAL_STRING(names[i],
                             textField(site.uplink.published()[i], "alert_level").c_str());
  }
}

static void test_an_acknowledgement_is_forwarded_naming_the_alert_it_answers() {
  QuietSite site;
  const Packet evacuate = site.transmit(site.wadiNodeOnAir, alert(AlertLevel::Evacuate));

  const Packet sent = site.transmit(site.sirenNodeOnAir, acknowledgement(evacuate));
  const uint32_t receivedAt = site.wallClock.unixSeconds();

  TEST_ASSERT_EQUAL_UINT(2, site.uplink.published().size());
  TEST_ASSERT_EQUAL_STRING(
      (messageStart(sent, receivedAt) + ",\"wadi_node\":" + number(evacuate.sender) +
       ",\"alert_level\":\"evacuate\",\"alert_counter\":" + number(evacuate.counter) + "}")
          .c_str(),
      site.uplink.published()[1].c_str());
}

static void test_a_wadi_node_heartbeat_is_forwarded_with_its_health_and_last_level() {
  QuietSite site;
  const uint16_t battery =
      static_cast<uint16_t>(lowBatteryMillivolts(site, site.wadiNodeOnAir.id) + 400);
  Packet packet = heartbeat(battery);
  packet.heartbeat.signalStrengthDbm = -97;
  packet.heartbeat.hasLastLevel = true;
  packet.heartbeat.lastLevelMm = 845;
  packet.heartbeat.discardedReadings = 2;

  const Packet sent = site.transmit(site.wadiNodeOnAir, packet);
  const uint32_t receivedAt = site.wallClock.unixSeconds();

  TEST_ASSERT_EQUAL_UINT(1, site.uplink.published().size());
  TEST_ASSERT_EQUAL_STRING(
      (messageStart(sent, receivedAt) + ",\"battery_mv\":" + number(battery) +
       R"(,"rssi_dbm":-97,"low_battery":false,"tamper":false,"level_cm":84.5,"level_repeated":false,"discards":2,"refresh_lost":false})")
          .c_str(),
      site.uplink.published()[0].c_str());
}

// A wadi node whose sensor gives no usable reading keeps the level it last measured and
// goes on sending it. The backend must be able to tell that level from one just measured.
static void test_a_level_the_node_could_not_measure_again_is_forwarded_as_repeated() {
  QuietSite site;
  Packet lastReading = reading(845, 0);
  lastReading.reading.levelRepeated = true;
  Packet alertRefresh = alert(AlertLevel::Warning);
  alertRefresh.alert.levelMm = 845;
  alertRefresh.alert.refresh = true;
  alertRefresh.alert.levelRepeated = true;
  Packet lastLevel = heartbeat(lowBatteryMillivolts(site, site.wadiNodeOnAir.id));
  lastLevel.heartbeat.hasLastLevel = true;
  lastLevel.heartbeat.lastLevelMm = 845;
  lastLevel.heartbeat.lastLevelRepeated = true;

  site.transmit(site.wadiNodeOnAir, lastReading);
  site.transmit(site.wadiNodeOnAir, alertRefresh);
  site.transmit(site.wadiNodeOnAir, lastLevel);

  TEST_ASSERT_EQUAL_UINT(3, site.uplink.published().size());
  for (const std::string& message : site.uplink.published()) {
    TEST_ASSERT_TRUE_MESSAGE(flagField(message, "level_repeated"), message.c_str());
  }
}

static void test_a_siren_node_heartbeat_is_forwarded_with_its_flags_and_no_level() {
  QuietSite site;
  const uint16_t battery =
      static_cast<uint16_t>(lowBatteryMillivolts(site, site.sirenNodeOnAir.id) + 600);
  Packet packet = heartbeat(battery);
  packet.heartbeat.signalStrengthDbm = -88;
  packet.heartbeat.tamper = true;
  packet.heartbeat.alertRefreshLost = true;

  const Packet sent = site.transmit(site.sirenNodeOnAir, packet);
  const uint32_t receivedAt = site.wallClock.unixSeconds();

  // A node that has measured no level reports none: the message has no level either.
  TEST_ASSERT_EQUAL_STRING(
      (messageStart(sent, receivedAt) + ",\"battery_mv\":" + number(battery) +
       R"(,"rssi_dbm":-88,"low_battery":false,"tamper":true,"discards":0,"refresh_lost":true})")
          .c_str(),
      site.uplink.published().back().c_str());
}

static void test_a_battery_below_the_threshold_of_its_node_is_reported_as_low() {
  QuietSite site;
  const uint16_t wadiNodeLow = lowBatteryMillivolts(site, site.wadiNodeOnAir.id);
  const uint16_t sirenNodeLow = lowBatteryMillivolts(site, site.sirenNodeOnAir.id);

  site.transmit(site.wadiNodeOnAir, heartbeat(wadiNodeLow));
  site.transmit(site.wadiNodeOnAir, heartbeat(static_cast<uint16_t>(wadiNodeLow - 1)));
  site.transmit(site.sirenNodeOnAir, heartbeat(sirenNodeLow));
  site.transmit(site.sirenNodeOnAir, heartbeat(static_cast<uint16_t>(sirenNodeLow - 1)));

  const std::vector<std::string>& published = site.uplink.published();
  TEST_ASSERT_EQUAL_UINT(4, published.size());
  TEST_ASSERT_FALSE(flagField(published[0], "low_battery"));
  TEST_ASSERT_TRUE(flagField(published[1], "low_battery"));
  // Each node is judged against its own threshold: the two batteries are not alike.
  TEST_ASSERT_FALSE(flagField(published[2], "low_battery"));
  TEST_ASSERT_TRUE(flagField(published[3], "low_battery"));
}

static void test_the_second_wadi_node_is_judged_against_a_battery_threshold_of_its_own() {
  QuietSite site;
  TestRadio& downstream = site.downstreamWadiNodeOnAir;
  const uint16_t low = lowBatteryMillivolts(site, downstream.id);

  site.transmit(downstream, heartbeat(low));
  site.transmit(downstream, heartbeat(static_cast<uint16_t>(low - 1)));

  const std::vector<std::string>& published = site.uplink.published();
  TEST_ASSERT_EQUAL_UINT(2, published.size());
  TEST_ASSERT_EQUAL_UINT(downstream.id, numberField(published[0], "node"));
  TEST_ASSERT_FALSE(flagField(published[0], "low_battery"));
  TEST_ASSERT_TRUE(flagField(published[1], "low_battery"));
}

static void test_the_all_clear_the_gateway_transmits_is_forwarded_naming_the_wadi_node_and_the_command() {
  QuietSite site;

  // The gateway does not hear itself: what it forwards is its own packet, dated with
  // the time it first transmitted it.
  const uint32_t command = site.issueAllClear();
  std::vector<uint32_t> transmittedAt;
  for (uint64_t elapsed = 0; elapsed < site.allClearSettleMs(); elapsed += TestSite::kStepMs) {
    site.runMs(TestSite::kStepMs);
    while (transmittedAt.size() < site.packetsOnAir(PacketKind::AllClear).size()) {
      transmittedAt.push_back(site.wallClock.unixSeconds());
    }
  }
  site.runMs(kUplinkPromptMs);

  // One packet and one message for each wadi node of the wadi.
  const std::vector<Packet> sent = site.packetsOnAir(PacketKind::AllClear);
  TEST_ASSERT_EQUAL_UINT(2, sent.size());
  TEST_ASSERT_EQUAL_UINT(sent.size(), site.uplink.published().size());
  TEST_ASSERT_TRUE(sent[0].allClear.wadiNode != sent[1].allClear.wadiNode);
  for (size_t i = 0; i < sent.size(); ++i) {
    TEST_ASSERT_EQUAL_UINT16(site.config.gateway.id, sent[i].sender);
    TEST_ASSERT_EQUAL_STRING(
        (messageStart(sent[i], transmittedAt[i]) + ",\"wadi_node\":" +
         number(sent[i].allClear.wadiNode) + ",\"command\":" + number(command) + "}")
            .c_str(),
        site.uplink.published()[i].c_str());
  }
}

static void test_a_test_packet_is_forwarded_as_a_test_message() {
  QuietSite site;

  const Packet sent = site.transmit(site.sirenNodeOnAir, testPacket());
  const uint32_t receivedAt = site.wallClock.unixSeconds();

  TEST_ASSERT_EQUAL_UINT(1, site.uplink.published().size());
  TEST_ASSERT_EQUAL_STRING((messageStart(sent, receivedAt) + "}").c_str(),
                           site.uplink.published()[0].c_str());
}

static void test_no_message_is_longer_than_the_contract_allows() {
  QuietSite site;
  // The packet with the most to say, saying it at full width.
  Packet packet = heartbeat(UINT16_MAX);
  packet.heartbeat.signalStrengthDbm = INT16_MIN;
  packet.heartbeat.hasLastLevel = true;
  packet.heartbeat.lastLevelMm = UINT16_MAX;
  packet.heartbeat.discardedReadings = UINT8_MAX;
  packet.heartbeat.tamper = true;
  packet.heartbeat.alertRefreshLost = true;

  site.transmit(site.wadiNodeOnAir, packet);

  TEST_ASSERT_EQUAL_UINT(1, site.uplink.published().size());
  TEST_ASSERT_TRUE(site.uplink.published()[0].size() <= wadiwatch::kMaxUplinkMessageLength);
}

// --- Only what the gateway accepts, once, with the time it was received -------------------

static void test_packets_the_gateway_refuses_are_not_forwarded() {
  QuietSite site;
  SiteAuth strangersKey = site.config.auth;
  strangersKey.key[0] ^= 0xFF;

  // Signed with another key; signed with the site key by a node outside the site;
  // not a packet at all.
  site.air.inject(signedReading(strangersKey, site.wadiNodeOnAir.id));
  site.air.inject(signedReading(site.config.auth, site.nodeIdOutsideSite()));
  site.air.inject({0xFF, 0x00, 0x13, 0x37});
  quietSpell(site, 2);
  TEST_ASSERT_EQUAL_UINT(0, site.uplink.attempts());

  // A genuine packet, and then the same bytes again from a recorder.
  const Packet genuine = site.transmit(site.wadiNodeOnAir, reading(500));
  site.air.inject(site.wadiNodeOnAir.lastFrame);
  quietSpell(site, 2);

  assertForwarded({genuine}, site);
}

static void test_an_alert_transmitted_three_times_is_forwarded_once() {
  QuietSite site;
  TestRadio& wadiNode = site.wadiNodeOnAir;

  // A wadi node sends each alert several times, the same bytes, some seconds apart,
  // and nothing else in between.
  const Packet warning = site.transmit(wadiNode, alert(AlertLevel::Warning));
  const Frame transmission = wadiNode.lastFrame;
  for (int repeat = 0; repeat < 2; ++repeat) {
    site.runMs(2 * TestSite::kStepMs);
    wadiNode.radio.send(transmission.data(), transmission.size());
  }
  const Packet next = site.transmit(wadiNode, reading(1040, 51));
  letTheUplinkCatchUp(site);

  assertForwarded({warning, next}, site);
}

static void test_an_alert_whose_first_transmission_is_lost_is_forwarded_once_from_a_repeat() {
  QuietSite site;
  TestRadio& wadiNode = site.wadiNodeOnAir;
  site.uplink.startOutage();

  site.air.dropNext(1);
  const Packet warning = site.transmit(wadiNode, alert(AlertLevel::Warning));
  const Frame transmission = wadiNode.lastFrame;
  site.runMs(2 * TestSite::kStepMs);
  wadiNode.radio.send(transmission.data(), transmission.size());
  site.runMs(TestSite::kStepMs);
  const uint32_t heardAt = site.wallClock.unixSeconds();
  site.runMs(2 * TestSite::kStepMs);
  wadiNode.radio.send(transmission.data(), transmission.size());
  quietSpell(site, 2);
  endOutage(site);

  // Stored once, and dated when the gateway first heard it.
  assertForwarded({warning}, site);
  TEST_ASSERT_EQUAL_UINT32(heardAt, timeOf(site.uplink.published()[0]));
}

static void test_each_packet_of_a_flood_is_forwarded_once_while_the_uplink_is_up() {
  TestSite site;

  floodTo(site, AlertLevel::Watch);
  floodTo(site, AlertLevel::Warning);
  floodTo(site, AlertLevel::Evacuate);
  // Past the next heartbeat.
  site.runMs(site.config.wadiNode.heartbeatIntervalMs);
  letTheUplinkCatchUp(site);

  TEST_ASSERT_TRUE(site.packetsOnAir(PacketKind::Alert).size() >= 3);
  TEST_ASSERT_TRUE(site.packetsOnAir(PacketKind::Heartbeat).size() >= 1);
  assertForwarded(site.packetsOnAir(), site);
  // Nothing had to be offered twice.
  TEST_ASSERT_EQUAL_UINT(site.uplink.published().size(), site.uplink.attempts());
}

static void test_with_both_wadi_nodes_sending_their_alerts_again_each_alert_is_forwarded_once() {
  TestSite site;
  const NodeId upstream = site.config.wadiNode.id;
  const NodeId downstream = site.config.downstreamWadiNode.id;

  floodBothWadiNodes(site);
  const uint64_t refreshes = 30;
  site.runMs(refreshes * site.config.wadiNode.alertRefreshIntervalMs);
  letTheUplinkCatchUp(site);

  // Each wadi node has sent its alert again every minute, three transmissions a time.
  TEST_ASSERT_TRUE(site.packetsOnAir(PacketKind::Alert, upstream).size() > refreshes);
  TEST_ASSERT_TRUE(site.packetsOnAir(PacketKind::Alert, downstream).size() > refreshes);
  // One message for each alert, each acknowledgement and each reading, whichever
  // node it came from, and nothing had to be offered twice.
  assertForwarded(site.packetsOnAir(), site);
  TEST_ASSERT_EQUAL_UINT(site.uplink.published().size(), site.uplink.attempts());
}

static void test_an_alert_reaches_the_backend_within_seconds_of_the_wadi_node_raising_it() {
  TestSite site;

  floodTo(site, AlertLevel::Warning);
  const uint32_t raisedAt = site.wallClock.unixSeconds();
  const Packet raised = site.packetsOnAir(PacketKind::Alert).back();
  site.runMs(kUplinkPromptMs);

  const std::string message = messageFor(raised, site);
  TEST_ASSERT_EQUAL_STRING("warning", textField(message, "alert_level").c_str());
  TEST_ASSERT_EQUAL_UINT32(raisedAt, timeOf(message));
}

static void test_a_message_carries_the_time_the_gateway_received_the_packet() {
  QuietSite site;

  site.transmit(site.wadiNodeOnAir, reading(300));
  const uint32_t firstReceivedAt = site.wallClock.unixSeconds();
  quietSpell(site, 14);
  site.uplink.startOutage();
  site.transmit(site.wadiNodeOnAir, reading(350));
  const uint32_t secondReceivedAt = site.wallClock.unixSeconds();
  quietSpell(site, 20);
  endOutage(site);

  const std::vector<std::string>& published = site.uplink.published();
  TEST_ASSERT_EQUAL_UINT(2, published.size());
  TEST_ASSERT_EQUAL_UINT32(firstReceivedAt, timeOf(published[0]));
  // Not the time it was finally forwarded, ten minutes later.
  TEST_ASSERT_EQUAL_UINT32(secondReceivedAt, timeOf(published[1]));
  TEST_ASSERT_TRUE(secondReceivedAt > firstReceivedAt);
}

// --- Uplink outage -----------------------------------------------------------------------

static void test_messages_from_an_uplink_outage_arrive_in_order_once_it_ends() {
  QuietSite site;
  std::vector<Packet> sent;
  std::vector<uint32_t> receivedAt;
  const auto transmit = [&](TestRadio& from, const Packet& packet) {
    sent.push_back(site.transmit(from, packet));
    receivedAt.push_back(site.wallClock.unixSeconds());
    quietSpell(site, 2);
  };

  transmit(site.wadiNodeOnAir, reading(300, 10));
  TEST_ASSERT_EQUAL_UINT(1, site.uplink.published().size());

  site.uplink.startOutage();
  transmit(site.wadiNodeOnAir, reading(700, 60));
  transmit(site.wadiNodeOnAir, alert(AlertLevel::Warning));
  transmit(site.sirenNodeOnAir, acknowledgement(sent.back()));
  transmit(site.sirenNodeOnAir, heartbeat(12000));
  transmit(site.wadiNodeOnAir, alert(AlertLevel::Evacuate));
  transmit(site.sirenNodeOnAir, acknowledgement(sent.back()));
  transmit(site.sirenNodeOnAir, testPacket());
  transmit(site.wadiNodeOnAir, heartbeat(3900));
  transmit(site.sirenNodeOnAir, allClear(site.wadiNodeOnAir.id));
  transmit(site.wadiNodeOnAir, reading(100, -20));
  // Nothing got through, and nothing was given up on.
  TEST_ASSERT_EQUAL_UINT(1, site.uplink.published().size());

  endOutage(site);

  // None lost, none twice, in the order the packets were received.
  assertForwarded(sent, site);
  for (size_t i = 0; i < sent.size(); ++i) {
    TEST_ASSERT_EQUAL_UINT32(receivedAt[i], timeOf(site.uplink.published()[i]));
  }
}

static void test_a_flood_during_an_uplink_outage_is_complete_on_the_backend_afterwards() {
  TestSite site;
  floodTo(site, AlertLevel::Watch);
  site.runMs(2ull * site.config.wadiNode.mode.stormMeasureIntervalMs);
  const size_t beforeTheOutage = site.uplink.published().size();
  TEST_ASSERT_TRUE(beforeTheOutage >= 1);

  site.uplink.startOutage();
  floodTo(site, AlertLevel::Warning);
  floodTo(site, AlertLevel::Evacuate);
  site.runMs(site.config.wadiNode.heartbeatIntervalMs);
  TEST_ASSERT_EQUAL_UINT(beforeTheOutage, site.uplink.published().size());
  TEST_ASSERT_TRUE(site.packetsOnAir().size() >= beforeTheOutage + 3);

  endOutage(site);

  assertForwarded(site.packetsOnAir(), site);
}

static void test_messages_that_arrive_after_the_outage_wait_their_turn_behind_the_stored_ones() {
  QuietSite site;
  site.uplink.startOutage();
  std::vector<Packet> sent;
  for (uint16_t i = 0; i < 20; ++i) {
    sent.push_back(site.transmit(site.wadiNodeOnAir, reading(static_cast<uint16_t>(100 + i))));
  }
  site.runMs(site.config.gateway.uplinkRetryIntervalMs);

  // The uplink returns while packets keep coming.
  site.uplink.endOutage();
  for (uint16_t i = 0; i < 20; ++i) {
    sent.push_back(site.transmit(site.wadiNodeOnAir, alert(AlertLevel::Warning)));
  }
  letTheUplinkCatchUp(site);

  assertForwarded(sent, site);
}

static void test_during_an_uplink_outage_the_gateway_retries_at_the_configured_interval() {
  QuietSite site;
  const uint64_t retryIntervalMs = site.config.gateway.uplinkRetryIntervalMs;
  site.uplink.startOutage();
  site.transmit(site.wadiNodeOnAir, reading(300));
  site.transmit(site.wadiNodeOnAir, reading(310));
  site.runMs(retryIntervalMs);

  const size_t attemptsBefore = site.uplink.attempts();
  const uint64_t intervals = 10;
  site.runMs(intervals * retryIntervalMs);

  // It keeps trying, but one message per interval rather than hammering the uplink.
  const size_t attempts = site.uplink.attempts() - attemptsBefore;
  TEST_ASSERT_TRUE(attempts >= intervals - 1);
  TEST_ASSERT_TRUE(attempts <= intervals + 1);
}

static void test_sms_are_still_sent_during_an_uplink_outage() {
  TestSite cutOff;
  TestSite connected;
  cutOff.uplink.startOutage();

  for (TestSite* site : {&cutOff, &connected}) {
    floodTo(*site, AlertLevel::Warning);
    site->runMs(kSmsTargetMs);
    floodTo(*site, AlertLevel::Evacuate);
    site->runMs(kSmsTargetMs);
  }

  // The same SMS, to the same recipients, as with the uplink working.
  const std::vector<Sms>& sent = cutOff.modem.sent();
  const std::vector<Sms>& expected = connected.modem.sent();
  TEST_ASSERT_TRUE(expected.size() > 0);
  TEST_ASSERT_EQUAL_UINT(expected.size(), sent.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    TEST_ASSERT_EQUAL_STRING(expected[i].phoneNumber.c_str(), sent[i].phoneNumber.c_str());
    TEST_ASSERT_EQUAL_STRING(expected[i].text.c_str(), sent[i].text.c_str());
  }
  TEST_ASSERT_EQUAL_UINT(0, cutOff.uplink.published().size());
}

static void test_messages_are_still_forwarded_during_a_gsm_outage() {
  TestSite site;
  site.modem.startOutage();

  floodTo(site, AlertLevel::Warning);
  const Packet raised = site.packetsOnAir(PacketKind::Alert).back();
  site.runMs(kUplinkPromptMs);
  // The alert is with the backend while its SMS are still waiting for the network.
  messageFor(raised, site);
  letTheUplinkCatchUp(site);

  TEST_ASSERT_EQUAL_UINT(0, site.modem.sent().size());
  TEST_ASSERT_TRUE(site.modem.refused().size() > 0);
  assertForwarded(site.packetsOnAir(), site);
}

// --- Restart -----------------------------------------------------------------------------

static void test_stored_messages_survive_a_gateway_restart() {
  QuietSite site;
  site.uplink.startOutage();
  std::vector<Packet> sent;
  sent.push_back(site.transmit(site.wadiNodeOnAir, reading(900, 55)));
  sent.push_back(site.transmit(site.wadiNodeOnAir, alert(AlertLevel::Warning)));
  sent.push_back(site.transmit(site.sirenNodeOnAir, acknowledgement(sent.back())));
  const uint32_t lastReceivedAt = site.wallClock.unixSeconds();
  quietSpell(site, 6);

  site.restartGateway();
  quietSpell(site, 6);
  sent.push_back(site.transmit(site.wadiNodeOnAir, alert(AlertLevel::Evacuate)));
  TEST_ASSERT_EQUAL_UINT(0, site.uplink.published().size());

  endOutage(site);

  // What was stored before the restart comes first, dated as it was received.
  assertForwarded(sent, site);
  TEST_ASSERT_EQUAL_UINT32(lastReceivedAt, timeOf(site.uplink.published()[2]));
}

static void test_a_restarted_gateway_does_not_forward_again_what_the_backend_has() {
  QuietSite site;
  std::vector<Packet> sent;
  sent.push_back(site.transmit(site.wadiNodeOnAir, reading(900)));
  sent.push_back(site.transmit(site.wadiNodeOnAir, alert(AlertLevel::Warning)));
  TEST_ASSERT_EQUAL_UINT(2, site.uplink.published().size());

  site.restartGateway();
  sent.push_back(site.transmit(site.wadiNodeOnAir, reading(950)));
  letTheUplinkCatchUp(site);

  assertForwarded(sent, site);
}

static void test_a_message_damaged_in_the_store_is_not_forwarded() {
  QuietSite damaged;
  QuietSite intact;
  for (QuietSite* site : {&damaged, &intact}) {
    site->uplink.startOutage();
    site->transmit(site->wadiNodeOnAir, reading(900, 55));
    site->transmit(site->wadiNodeOnAir, alert(AlertLevel::Warning));
    site->transmit(site->sirenNodeOnAir, heartbeat(12000));
  }

  // The gateway loses power, and one stored message does not come back as written.
  TEST_ASSERT_TRUE(damaged.gatewayMessageStorage.damageOneSlot());
  damaged.restartGateway();
  endOutage(damaged);
  endOutage(intact);

  // One message is gone. The others arrive exactly as they would have.
  TEST_ASSERT_EQUAL_UINT(3, intact.uplink.published().size());
  TEST_ASSERT_EQUAL_UINT(2, damaged.uplink.published().size());
  for (const std::string& message : damaged.uplink.published()) {
    bool known = false;
    for (const std::string& original : intact.uplink.published()) {
      if (message == original) known = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(known, message.c_str());
  }
}

// --- A full store ------------------------------------------------------------------------

static void test_when_the_store_is_full_the_oldest_reading_gives_way_to_a_new_one() {
  CutOffSite site(3);
  site.fromWadiNode(reading(100));
  const Packet second = site.fromWadiNode(reading(200));
  const Packet third = site.fromWadiNode(reading(300));

  const Packet fourth = site.fromWadiNode(reading(400));
  endOutage(site);

  assertForwarded({second, third, fourth}, site);
}

static void test_a_reading_gives_way_before_a_heartbeat_or_an_acknowledgement() {
  CutOffSite site(3);
  const Packet heartbeatKept = site.fromWadiNode(heartbeat(3900));
  const Packet testKept = site.fromSirenNode(testPacket());
  // The newest of the three, and still the one to go.
  site.fromWadiNode(reading(100));

  const Packet warning = site.fromWadiNode(alert(AlertLevel::Warning));
  endOutage(site);

  assertForwarded({heartbeatKept, testKept, warning}, site);
}

static void test_a_heartbeat_gives_way_before_an_acknowledgement_or_a_test() {
  CutOffSite site(3);
  const Packet testKept = site.fromSirenNode(testPacket());
  site.fromWadiNode(heartbeat(3900));
  const Packet warning = site.fromWadiNode(alert(AlertLevel::Warning));

  const Packet acknowledged = site.fromSirenNode(acknowledgement(warning));
  endOutage(site);

  assertForwarded({testKept, warning, acknowledged}, site);
}

static void test_an_alert_is_never_dropped_to_make_room_for_another_kind_of_message() {
  CutOffSite site(3);
  const Packet watch = site.fromWadiNode(alert(AlertLevel::Watch));
  const Packet warning = site.fromWadiNode(alert(AlertLevel::Warning));
  const Packet evacuate = site.fromWadiNode(alert(AlertLevel::Evacuate));

  site.fromWadiNode(reading(1600));
  site.fromWadiNode(heartbeat(3900));
  site.fromSirenNode(acknowledgement(evacuate));
  site.fromSirenNode(testPacket());
  endOutage(site);

  assertForwarded({watch, warning, evacuate}, site);
}

static void test_alerts_take_the_place_of_everything_else_in_a_full_store() {
  CutOffSite site(3);
  site.fromWadiNode(reading(700));
  site.fromWadiNode(heartbeat(3900));
  site.fromSirenNode(testPacket());

  const Packet watch = site.fromWadiNode(alert(AlertLevel::Watch));
  const Packet warning = site.fromWadiNode(alert(AlertLevel::Warning));
  const Packet evacuate = site.fromWadiNode(alert(AlertLevel::Evacuate));
  endOutage(site);

  assertForwarded({watch, warning, evacuate}, site);
}

static void test_when_only_alerts_fit_every_change_of_alert_level_is_kept() {
  CutOffSite site(4);
  const Packet watch = site.fromWadiNode(alert(AlertLevel::Watch));
  const Packet warning = site.fromWadiNode(alert(AlertLevel::Warning));
  // The refresh a wadi node at Warning sends every minute.
  site.fromWadiNode(alert(AlertLevel::Warning));
  site.fromWadiNode(alert(AlertLevel::Warning));

  // Each new message takes the place of a refresh, never of a change.
  const Packet evacuate = site.fromWadiNode(alert(AlertLevel::Evacuate));
  site.fromWadiNode(alert(AlertLevel::Evacuate));
  const Packet cleared = site.fromSirenNode(allClear(site.wadiNodeOnAir.id));
  endOutage(site);

  assertForwarded({watch, warning, evacuate, cleared}, site);
}

static void test_a_store_full_of_changes_of_alert_level_does_not_keep_a_refresh() {
  CutOffSite site(3);
  const Packet watch = site.fromWadiNode(alert(AlertLevel::Watch));
  const Packet warning = site.fromWadiNode(alert(AlertLevel::Warning));
  const Packet evacuate = site.fromWadiNode(alert(AlertLevel::Evacuate));

  site.fromWadiNode(alert(AlertLevel::Evacuate));
  endOutage(site);

  assertForwarded({watch, warning, evacuate}, site);
}

static void test_an_alert_after_an_all_clear_is_a_change_even_at_the_level_before_it() {
  CutOffSite site(3);
  const Packet watch = site.fromWadiNode(alert(AlertLevel::Watch));
  const Packet cleared = site.fromSirenNode(allClear(site.wadiNodeOnAir.id));
  // A new event begins at the level the last one had.
  const Packet watchAgain = site.fromWadiNode(alert(AlertLevel::Watch));

  site.fromWadiNode(alert(AlertLevel::Watch));
  endOutage(site);

  assertForwarded({watch, cleared, watchAgain}, site);
}

static void test_a_store_holding_only_changes_of_alert_level_gives_up_the_oldest() {
  CutOffSite site(2);
  site.fromWadiNode(alert(AlertLevel::Watch));
  const Packet warning = site.fromWadiNode(alert(AlertLevel::Warning));

  // Something has to go. The backend must still learn how things stand now.
  const Packet evacuate = site.fromWadiNode(alert(AlertLevel::Evacuate));
  endOutage(site);

  assertForwarded({warning, evacuate}, site);
}

static void test_a_refresh_of_an_alert_the_backend_has_does_not_count_as_a_change() {
  QuietSite site;
  site.gatewayMessageStorage.setSlotCount(2);
  TestRadio& upstream = site.wadiNodeOnAir;
  TestRadio& downstream = site.downstreamWadiNodeOnAir;
  // The upstream wadi node reaches Warning while the uplink works: the backend has it,
  // and the store is empty again.
  const Packet warningUpstream = site.transmit(upstream, alert(AlertLevel::Warning));
  TEST_ASSERT_EQUAL_UINT(1, site.uplink.published().size());

  site.uplink.startOutage();
  const Packet watch = site.transmit(downstream, alert(AlertLevel::Watch));
  // The upstream wadi node says Warning again, as it does every minute. It tells the
  // backend nothing new, although no alert of that node is waiting in the store.
  site.transmit(upstream, alert(AlertLevel::Warning));
  const Packet warning = site.transmit(downstream, alert(AlertLevel::Warning));
  endOutage(site);

  // It is the refresh that gave way, not the downstream wadi node's Watch.
  assertForwarded({warningUpstream, watch, warning}, site);
}

static void test_a_refresh_tells_the_backend_of_a_change_that_a_full_store_gave_up() {
  CutOffSite site(2);
  TestRadio& upstream = site.wadiNodeOnAir;
  TestRadio& downstream = site.downstreamWadiNodeOnAir;
  // Three changes of alert level and room for two: the oldest goes, and with it all
  // the backend would have heard of the upstream wadi node.
  site.transmit(upstream, alert(AlertLevel::Warning));
  site.transmit(downstream, alert(AlertLevel::Watch));
  const Packet warning = site.transmit(downstream, alert(AlertLevel::Warning));

  // To the backend the upstream wadi node's next refresh is news, so it is kept as a
  // change is, at the cost of the oldest change: the backend must still learn how
  // things stand now.
  const Packet refresh = site.transmit(upstream, alert(AlertLevel::Warning));
  // The one after it is a refresh again.
  site.transmit(upstream, alert(AlertLevel::Warning));
  endOutage(site);

  assertForwarded({warning, refresh}, site);
}

static void test_after_a_restart_the_first_alert_heard_from_a_wadi_node_is_kept_as_a_change() {
  QuietSite site;
  site.gatewayMessageStorage.setSlotCount(2);
  TestRadio& upstream = site.wadiNodeOnAir;
  const Packet before = site.transmit(upstream, alert(AlertLevel::Warning));
  TEST_ASSERT_EQUAL_UINT(1, site.uplink.published().size());

  // The gateway no longer knows what the backend has been told.
  site.restartGateway();
  site.uplink.startOutage();
  const Packet first = site.transmit(upstream, alert(AlertLevel::Warning));
  site.transmit(upstream, alert(AlertLevel::Warning));
  const Packet evacuate = site.transmit(upstream, alert(AlertLevel::Evacuate));
  endOutage(site);

  assertForwarded({before, first, evacuate}, site);
}

static void test_two_wadi_nodes_refreshing_their_alerts_through_a_long_outage_cost_no_change() {
  TestSite site;
  // Room for a few minutes of what the site sends while both wadi nodes are in alert.
  const size_t slots = 12;
  site.gatewayMessageStorage.setSlotCount(slots);
  site.uplink.startOutage();

  floodBothWadiNodes(site);
  site.runMs(2ull * site.config.wadiNode.heartbeatIntervalMs);
  // Far more than the store holds: an alert, a reading and an acknowledgement a minute
  // for each wadi node.
  const size_t sentDuringTheOutage = site.packetsOnAir().size();
  TEST_ASSERT_TRUE(sentDuringTheOutage > 10 * slots);
  const std::vector<Packet> changes = changesOfAlertLevelOnAir(site);
  // Warning at each wadi node, then Evacuate downstream.
  TEST_ASSERT_EQUAL_UINT(3, changes.size());

  endOutage(site);

  // The backend learns of every change of alert level, however many refreshes followed.
  for (const Packet& change : changes) messageFor(change, site);
  assertForwardedInOrderOnceEach(site);
  // What gave way were refreshes, acknowledgements and readings.
  TEST_ASSERT_TRUE(site.uplink.published().size() < site.packetsOnAir().size());
}

// --- Before the gateway knows the time ---------------------------------------------------

static void test_packets_received_before_the_time_is_known_are_dated_once_it_is() {
  QuietSite site;
  site.wallClock.forgetTime();

  std::vector<Packet> sent;
  sent.push_back(site.transmit(site.wadiNodeOnAir, reading(300)));
  const uint32_t firstReceivedAt = site.wallClock.unixSeconds();
  quietSpell(site, 14);
  sent.push_back(site.transmit(site.wadiNodeOnAir, alert(AlertLevel::Watch)));
  const uint32_t secondReceivedAt = site.wallClock.unixSeconds();
  quietSpell(site, 6);
  // The gateway cannot date them, so it holds them.
  TEST_ASSERT_EQUAL_UINT(0, site.uplink.attempts());

  site.wallClock.learnTime();
  letTheUplinkCatchUp(site);

  assertForwarded(sent, site);
  TEST_ASSERT_EQUAL_UINT32(firstReceivedAt, timeOf(site.uplink.published()[0]));
  TEST_ASSERT_EQUAL_UINT32(secondReceivedAt, timeOf(site.uplink.published()[1]));
}

static void test_packets_held_undated_through_a_restart_are_dated_at_the_restart() {
  QuietSite site;
  site.wallClock.forgetTime();
  std::vector<Packet> sent;
  sent.push_back(site.transmit(site.wadiNodeOnAir, reading(300)));
  quietSpell(site, 10);

  site.restartGateway();
  site.runMs(TestSite::kStepMs);
  const uint32_t restartedAt = site.wallClock.unixSeconds();
  quietSpell(site, 8);
  sent.push_back(site.transmit(site.wadiNodeOnAir, alert(AlertLevel::Watch)));
  const uint32_t secondReceivedAt = site.wallClock.unixSeconds();
  quietSpell(site, 4);
  site.wallClock.learnTime();
  letTheUplinkCatchUp(site);

  // How long before the restart the first packet came is lost with the restart. The
  // restart is the latest it can have been, and that is the time it is given.
  assertForwarded(sent, site);
  TEST_ASSERT_EQUAL_UINT32(restartedAt, timeOf(site.uplink.published()[0]));
  TEST_ASSERT_EQUAL_UINT32(secondReceivedAt, timeOf(site.uplink.published()[1]));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_reading_is_forwarded_with_everything_the_packet_carries);
  RUN_TEST(test_a_reading_of_a_falling_dry_wadi_is_forwarded_exactly);
  RUN_TEST(test_the_largest_values_a_reading_can_carry_are_forwarded_exactly);
  RUN_TEST(test_an_alert_is_forwarded_with_the_level_and_rate_of_rise_behind_it);
  RUN_TEST(test_an_alert_refresh_is_forwarded_as_one);
  RUN_TEST(test_every_alert_level_is_forwarded_under_its_contract_name);
  RUN_TEST(test_an_acknowledgement_is_forwarded_naming_the_alert_it_answers);
  RUN_TEST(test_a_wadi_node_heartbeat_is_forwarded_with_its_health_and_last_level);
  RUN_TEST(test_a_level_the_node_could_not_measure_again_is_forwarded_as_repeated);
  RUN_TEST(test_a_siren_node_heartbeat_is_forwarded_with_its_flags_and_no_level);
  RUN_TEST(test_a_battery_below_the_threshold_of_its_node_is_reported_as_low);
  RUN_TEST(test_the_second_wadi_node_is_judged_against_a_battery_threshold_of_its_own);
  RUN_TEST(test_the_all_clear_the_gateway_transmits_is_forwarded_naming_the_wadi_node_and_the_command);
  RUN_TEST(test_a_test_packet_is_forwarded_as_a_test_message);
  RUN_TEST(test_no_message_is_longer_than_the_contract_allows);
  RUN_TEST(test_packets_the_gateway_refuses_are_not_forwarded);
  RUN_TEST(test_an_alert_transmitted_three_times_is_forwarded_once);
  RUN_TEST(test_an_alert_whose_first_transmission_is_lost_is_forwarded_once_from_a_repeat);
  RUN_TEST(test_each_packet_of_a_flood_is_forwarded_once_while_the_uplink_is_up);
  RUN_TEST(test_with_both_wadi_nodes_sending_their_alerts_again_each_alert_is_forwarded_once);
  RUN_TEST(test_an_alert_reaches_the_backend_within_seconds_of_the_wadi_node_raising_it);
  RUN_TEST(test_a_message_carries_the_time_the_gateway_received_the_packet);
  RUN_TEST(test_messages_from_an_uplink_outage_arrive_in_order_once_it_ends);
  RUN_TEST(test_a_flood_during_an_uplink_outage_is_complete_on_the_backend_afterwards);
  RUN_TEST(test_messages_that_arrive_after_the_outage_wait_their_turn_behind_the_stored_ones);
  RUN_TEST(test_during_an_uplink_outage_the_gateway_retries_at_the_configured_interval);
  RUN_TEST(test_sms_are_still_sent_during_an_uplink_outage);
  RUN_TEST(test_messages_are_still_forwarded_during_a_gsm_outage);
  RUN_TEST(test_stored_messages_survive_a_gateway_restart);
  RUN_TEST(test_a_restarted_gateway_does_not_forward_again_what_the_backend_has);
  RUN_TEST(test_a_message_damaged_in_the_store_is_not_forwarded);
  RUN_TEST(test_when_the_store_is_full_the_oldest_reading_gives_way_to_a_new_one);
  RUN_TEST(test_a_reading_gives_way_before_a_heartbeat_or_an_acknowledgement);
  RUN_TEST(test_a_heartbeat_gives_way_before_an_acknowledgement_or_a_test);
  RUN_TEST(test_an_alert_is_never_dropped_to_make_room_for_another_kind_of_message);
  RUN_TEST(test_alerts_take_the_place_of_everything_else_in_a_full_store);
  RUN_TEST(test_when_only_alerts_fit_every_change_of_alert_level_is_kept);
  RUN_TEST(test_a_store_full_of_changes_of_alert_level_does_not_keep_a_refresh);
  RUN_TEST(test_an_alert_after_an_all_clear_is_a_change_even_at_the_level_before_it);
  RUN_TEST(test_a_store_holding_only_changes_of_alert_level_gives_up_the_oldest);
  RUN_TEST(test_a_refresh_of_an_alert_the_backend_has_does_not_count_as_a_change);
  RUN_TEST(test_a_refresh_tells_the_backend_of_a_change_that_a_full_store_gave_up);
  RUN_TEST(test_after_a_restart_the_first_alert_heard_from_a_wadi_node_is_kept_as_a_change);
  RUN_TEST(test_two_wadi_nodes_refreshing_their_alerts_through_a_long_outage_cost_no_change);
  RUN_TEST(test_packets_received_before_the_time_is_known_are_dated_once_it_is);
  RUN_TEST(test_packets_held_undated_through_a_restart_are_dated_at_the_restart);
  return UNITY_END();
}
