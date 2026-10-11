// Scenario tests for the all clear, end to end on the device side: the backend's
// command comes down the gateway's uplink, the gateway transmits a signed all clear for
// every wadi node of the wadi, the siren node stops, the wadi nodes drop their alert
// level, the closing SMS goes out and the backend is told.
//
// Every duration comes from the site configuration. The scenarios watch the site step
// by step, so they assume the configured durations are whole steps.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <unity.h>

#include "wadiwatch/alert_engine.h"
#include "wadiwatch/downlink_command.h"
#include "wadiwatch/level_measurement.h"
#include "wadiwatch/packet.h"
#include "wadiwatch/radio_config.h"
#include "wadiwatch/recipients.h"
#include "wadiwatch/site_config.h"
#include "wadiwatch/sms_templates.h"
#include "wadiwatch_testing/scripted_node.h"
#include "wadiwatch_testing/test_site.h"

using wadiwatch::AlertLevel;
using wadiwatch::AllClearCommand;
using wadiwatch::Language;
using wadiwatch::levelThresholdCm;
using wadiwatch::NodeId;
using wadiwatch::Packet;
using wadiwatch::PacketKind;
using wadiwatch::parseAllClearCommand;
using wadiwatch::Recipient;
using wadiwatch::RecipientGroup;
using wadiwatch::SiteAuth;
using wadiwatch::SiteConfig;
using wadiwatch::WadiConfig;
using wadiwatch::WadiId;
using wadiwatch::WadiNodeConfig;
using wadiwatch::testing::FakeLevelSensor;
using wadiwatch::testing::Frame;
using wadiwatch::testing::InMemoryRadio;
using wadiwatch::testing::ScriptedNode;
using wadiwatch::testing::SirenSound;
using wadiwatch::testing::Sms;
using wadiwatch::testing::TestSite;

void setUp() {}
void tearDown() {}

#define ASSERT_ALERT_LEVEL(expected, actual) \
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected), static_cast<uint8_t>(actual))

#define ASSERT_SIREN_SOUND(expected, actual) \
  TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual))

namespace {

const uint64_t kStepMs = TestSite::kStepMs;
const uint64_t kMinuteMs = 60ull * 1000;

// One of the site's two wadi nodes, with the hardware a scenario touches.
struct Node {
  const WadiNodeConfig& config;
  FakeLevelSensor& levelSensor;
  InMemoryRadio& radio;
};

// In the prototype site the first wadi node is upstream of the second.
Node upstreamNode(TestSite& site) {
  return Node{site.config.wadiNode, site.levelSensor, site.wadiNodeRadio};
}

Node downstreamNode(TestSite& site) {
  return Node{site.config.downstreamWadiNode, site.downstream.levelSensor, site.downstream.radio};
}

// A level that is clearly at `alertLevel` and clearly short of the next one up.
float levelWithin(const Node& node, AlertLevel alertLevel) {
  const wadiwatch::AlertThresholds& thresholds = node.config.alertThresholds;
  const float lowerCm = levelThresholdCm(thresholds, alertLevel);
  const float upperCm =
      alertLevel == AlertLevel::Evacuate
          ? thresholds.bankFullDepthCm
          : levelThresholdCm(thresholds,
                             static_cast<AlertLevel>(static_cast<uint8_t>(alertLevel) + 1));
  return (lowerCm + upperCm) / 2.0f;
}

std::vector<Packet> alertsFrom(const TestSite& site, const Node& node) {
  return site.packetsOnAir(PacketKind::Alert, node.config.id);
}

std::vector<Packet> readingsFrom(const TestSite& site, const Node& node) {
  return site.packetsOnAir(PacketKind::Reading, node.config.id);
}

// The wadi rises to `alertLevel` at a node and stays there until that node has put
// that alert level on the air, whichever operating mode it is in.
void floodTo(TestSite& site, const Node& node, AlertLevel alertLevel) {
  // An alert sent from now on: the node may have announced this level in an event
  // that an all clear has since ended.
  const size_t alertsBefore = alertsFrom(site, node).size();
  node.levelSensor.setWaterLevelCm(levelWithin(node, alertLevel));
  for (uint64_t waited = 0;; waited += kStepMs) {
    const std::vector<Packet> alerts = alertsFrom(site, node);
    if (alerts.size() > alertsBefore && alerts.back().alert.alertLevel == alertLevel) return;
    TEST_ASSERT_TRUE_MESSAGE(waited < node.config.mode.dryMeasureIntervalMs + kMinuteMs,
                             "the wadi node never raised the alert level");
    site.runMs(kStepMs);
  }
}

// The flood is over: the bed is dry again at both wadi nodes.
void waterRecedes(TestSite& site) {
  site.levelSensor.setWaterLevelCm(0);
  site.downstream.levelSensor.setWaterLevelCm(0);
  // Long enough for a node in storm mode to have measured the dry bed.
  site.runMs(2 * site.config.wadiNode.mode.stormMeasureIntervalMs);
}

// The officer issues the all clear and the site is given the time the gateway needs.
uint32_t issueAllClearAndWait(TestSite& site) {
  const uint32_t command = site.issueAllClear();
  site.runMs(site.allClearSettleMs());
  return command;
}

// The all clear packets the gateway has put on the air, each once however many times
// it was transmitted. They are read through the codec, as every device reads them:
// a packet listed here carries a valid authentication code.
std::vector<Packet> allClearsOnAir(const TestSite& site) {
  return site.packetsOnAir(PacketKind::AllClear, site.config.gateway.id);
}

std::vector<Packet> allClearsFor(const TestSite& site, const Node& node) {
  std::vector<Packet> named;
  for (const Packet& allClear : allClearsOnAir(site)) {
    if (allClear.allClear.wadiNode == node.config.id) named.push_back(allClear);
  }
  return named;
}

// Whether the siren sounds at any moment of the next `durationMs`.
bool sirenSoundsWithin(TestSite& site, uint64_t durationMs) {
  bool sounded = false;
  for (uint64_t elapsed = 0; elapsed < durationMs; elapsed += kStepMs) {
    site.runMs(kStepMs);
    sounded = sounded || site.siren.isOn();
  }
  return sounded;
}

// For how many of the next `steps` steps a node's receiver is on.
uint64_t stepsListening(TestSite& site, const Node& node, uint64_t steps) {
  uint64_t listening = 0;
  for (uint64_t step = 0; step < steps; ++step) {
    site.runMs(kStepMs);
    if (node.radio.isListening()) ++listening;
  }
  return listening;
}

// How many level measurements a node takes in the next `durationMs`.
size_t measurementsWithin(TestSite& site, const Node& node, uint64_t durationMs) {
  const size_t pingsBefore = node.levelSensor.readCount();
  site.runMs(durationMs);
  return (node.levelSensor.readCount() - pingsBefore) / wadiwatch::kPingsPerMeasurement;
}

// --- SMS ---

const Recipient& recipient(const TestSite& site, size_t slot) {
  return site.recipients.recipients[slot];
}

bool isRegistered(const Recipient& recipient) { return recipient.phoneNumber[0] != '\0'; }

const char* wadiNameIn(const WadiConfig& wadi, Language language) {
  return language == Language::Arabic ? wadi.nameArabic : wadi.nameFrench;
}

// The closing SMS a recipient should read at the all clear for `wadi`.
std::string closingText(const Recipient& recipient, const WadiConfig& wadi) {
  char text[wadiwatch::kMaxSmsTextLength + 1];
  const size_t length = wadiwatch::renderClosingSms(
      recipient.language, wadiNameIn(wadi, recipient.language), text, sizeof(text));
  return std::string(text, length);
}

std::vector<std::string> textsSentTo(const TestSite& site, const Recipient& recipient) {
  std::vector<std::string> texts;
  for (const Sms& sms : site.modem.sent()) {
    if (sms.phoneNumber == recipient.phoneNumber) texts.push_back(sms.text);
  }
  return texts;
}

// The SMS a recipient should read when `wadi` reaches `alertLevel`.
std::string alertText(const Recipient& recipient, const WadiConfig& wadi, AlertLevel alertLevel) {
  char text[wadiwatch::kMaxSmsTextLength + 1];
  const size_t length = wadiwatch::renderAlertSms(
      recipient.language, alertLevel, wadiNameIn(wadi, recipient.language), text, sizeof(text));
  return std::string(text, length);
}

size_t closingSmsCountTo(const TestSite& site, const Recipient& recipient) {
  size_t count = 0;
  const std::string closing = closingText(recipient, site.config.gateway.wadis[0]);
  for (const std::string& text : textsSentTo(site, recipient)) {
    if (text == closing) ++count;
  }
  return count;
}

size_t closingSmsCount(const TestSite& site) {
  size_t count = 0;
  for (size_t slot = 0; slot < wadiwatch::kMaxRecipients; ++slot) {
    if (isRegistered(recipient(site, slot))) count += closingSmsCountTo(site, recipient(site, slot));
  }
  return count;
}

// Long enough for the gateway to hand every waiting SMS to the modem.
void letSmsGoOut(TestSite& site) { site.runMs(kMinuteMs); }

// Whether a recipient of the committed list is told of Warning at the site's wadi.
bool toldOfWarning(const TestSite& site, const Recipient& recipient) {
  return recipient.group == RecipientGroup::Official ||
         recipient.nearestWadi == site.config.gateway.wadis[0].id;
}

// --- The uplink ---

bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

std::vector<std::string> allClearMessages(const TestSite& site) {
  std::vector<std::string> messages;
  for (const std::string& message : site.uplink.published()) {
    if (contains(message, "\"kind\":\"all_clear\"")) messages.push_back(message);
  }
  return messages;
}

// --- Sites ---

// The prototype site with each wadi node on a wadi of its own.
SiteConfig siteWithTwoWadis() {
  SiteConfig config = wadiwatch::prototypeSite();
  const WadiConfig first = {1, "وادي الاختبار", "Oued Essai", {config.wadiNode.id}, 1};
  const WadiConfig second = {2, "وادي الجوار", "Oued Voisin", {config.downstreamWadiNode.id}, 1};
  config.gateway.wadis[0] = first;
  config.gateway.wadis[1] = second;
  config.gateway.wadiCount = 2;
  // On different wadis neither node is upstream of the other.
  config.downstreamWadiNode.upstream.node = wadiwatch::kNoNode;
  return config;
}

bool parses(const std::string& text) {
  AllClearCommand command;
  return parseAllClearCommand(text.data(), text.size(), command);
}

}  // namespace

// --- The gateway relays the command as a signed packet ---

static void test_an_accepted_all_clear_is_transmitted_as_a_signed_packet_for_every_wadi_node_of_the_wadi() {
  TestSite site;
  floodTo(site, upstreamNode(site), AlertLevel::Warning);
  waterRecedes(site);
  TEST_ASSERT_EQUAL_UINT(0, allClearsOnAir(site).size());

  const uint32_t command = issueAllClearAndWait(site);

  // One packet for each wadi node of the wadi, whether or not it was in alert, from
  // the gateway and carrying the command's number.
  TEST_ASSERT_EQUAL_UINT(1, allClearsFor(site, upstreamNode(site)).size());
  TEST_ASSERT_EQUAL_UINT(1, allClearsFor(site, downstreamNode(site)).size());
  for (const Packet& allClear : allClearsOnAir(site)) {
    TEST_ASSERT_EQUAL_UINT16(site.config.gateway.id, allClear.sender);
    TEST_ASSERT_EQUAL_UINT32(command, allClear.allClear.command);
  }
  // Each goes out as many times as configured, like an alert.
  for (const Frame& frame : site.framesOnAir(PacketKind::AllClear)) {
    TEST_ASSERT_EQUAL_UINT(site.config.gateway.allClearRepeat.transmissions,
                           site.timesTransmitted(frame));
  }
}

static void test_without_a_command_the_gateway_never_transmits_an_all_clear() {
  TestSite site;
  floodTo(site, upstreamNode(site), AlertLevel::Warning);
  waterRecedes(site);

  // However long the wadi has been dry: the all clear is an officer's decision.
  site.runMs(2ull * site.config.sirenNode.alertRefreshTimeoutMs);

  TEST_ASSERT_EQUAL_UINT(0, site.packetsOnAir(PacketKind::AllClear).size());
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());
  ASSERT_ALERT_LEVEL(AlertLevel::Warning, site.latestAlertOnAir(site.config.wadiNode.id));
}

static void test_the_all_clear_for_one_wadi_leaves_the_event_at_another_wadi_alone() {
  TestSite site(siteWithTwoWadis());
  const Node first = upstreamNode(site);
  const Node second = downstreamNode(site);
  floodTo(site, first, AlertLevel::Warning);
  floodTo(site, second, AlertLevel::Warning);
  waterRecedes(site);

  site.issueAllClear(site.config.gateway.wadis[0].id);
  site.runMs(site.allClearSettleMs());

  TEST_ASSERT_EQUAL_UINT(1, allClearsFor(site, first).size());
  TEST_ASSERT_EQUAL_UINT(0, allClearsFor(site, second).size());
  // The second wadi's node goes on sending its alert, and the siren on sounding for it.
  const size_t alertsFromFirst = alertsFrom(site, first).size();
  const size_t alertsFromSecond = alertsFrom(site, second).size();
  site.runMs(5 * second.config.alertRefreshIntervalMs);
  TEST_ASSERT_EQUAL_UINT(alertsFromFirst, alertsFrom(site, first).size());
  TEST_ASSERT_TRUE(alertsFrom(site, second).size() >= alertsFromSecond + 4);
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());
}

// --- Siren nodes stop ---

static void test_the_siren_stops_on_the_accepted_all_clear_and_stays_silent() {
  const AlertLevel alertLevels[] = {AlertLevel::Warning, AlertLevel::Evacuate};
  const SirenSound sounds[] = {SirenSound::Intermittent, SirenSound::Continuous};
  for (size_t i = 0; i < 2; ++i) {
    TestSite site;
    floodTo(site, upstreamNode(site), alertLevels[i]);
    waterRecedes(site);
    // A falling level alone does not quieten the siren.
    ASSERT_SIREN_SOUND(sounds[i], site.listenToSiren());

    issueAllClearAndWait(site);

    ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
    TEST_ASSERT_FALSE(site.siren.isStrobeOn());
    // The wadi node is still there and has not been taken off the air: it is the all
    // clear it accepted that keeps it from sounding the siren again.
    TEST_ASSERT_FALSE(sirenSoundsWithin(site, site.config.sirenNode.alertRefreshTimeoutMs));
    TEST_ASSERT_FALSE(site.siren.isStrobeOn());
  }
}

static void test_the_siren_is_not_sounded_again_between_the_command_and_the_wadi_node_hearing_it() {
  TestSite site;
  floodTo(site, upstreamNode(site), AlertLevel::Evacuate);
  waterRecedes(site);

  site.issueAllClear();

  // From the moment the siren first falls silent it stays silent: the gateway held
  // the all clear until the wadi node was listening, so node and siren node heard
  // the same packet.
  uint64_t waitedMs = 0;
  while (site.siren.isOn()) {
    TEST_ASSERT_TRUE_MESSAGE(waitedMs < site.allClearSettleMs(), "the siren never stopped");
    site.runMs(kStepMs);
    waitedMs += kStepMs;
  }
  // And that is within one alert refresh interval of the command.
  TEST_ASSERT_TRUE(waitedMs <= site.config.wadiNode.alertRefreshIntervalMs + kStepMs);
  TEST_ASSERT_FALSE(sirenSoundsWithin(site, 10 * site.config.wadiNode.alertRefreshIntervalMs));
}

static void test_a_forged_all_clear_stops_neither_the_siren_nor_the_wadi_node() {
  TestSite site;
  const Node node = upstreamNode(site);
  floodTo(site, node, AlertLevel::Evacuate);
  waterRecedes(site);

  // Signed with another key, in the gateway's name.
  SiteAuth forgerAuth = site.config.auth;
  forgerAuth.key[0] ^= 0x01;
  ScriptedNode forger(forgerAuth, site.config.gateway.id);
  // Signed with the site key, by a transmitter that is not a node of the site.
  ScriptedNode outsider(site.config.auth, site.nodeIdOutsideSite());

  // Each is put on the air again and again for several minutes, so that the wadi
  // node, which listens only after its own transmissions, has every chance to hear it.
  for (uint64_t elapsed = 0; elapsed < 3 * node.config.alertRefreshIntervalMs; elapsed += kStepMs) {
    site.air.inject(forger.allClear(node.config.id));
    site.air.inject(outsider.allClear(node.config.id));
    site.runMs(kStepMs);
    TEST_ASSERT_TRUE(site.siren.isOn());
  }

  ASSERT_SIREN_SOUND(SirenSound::Continuous, site.listenToSiren());
  const size_t alertsBefore = alertsFrom(site, node).size();
  site.runMs(3 * node.config.alertRefreshIntervalMs);
  TEST_ASSERT_TRUE(alertsFrom(site, node).size() >= alertsBefore + 2);
  ASSERT_ALERT_LEVEL(AlertLevel::Evacuate, readingsFrom(site, node).back().reading.alertLevel);

  // Control: the gateway's own all clear stops both.
  issueAllClearAndWait(site);
  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
  ASSERT_ALERT_LEVEL(AlertLevel::None, readingsFrom(site, node).back().reading.alertLevel);
}

static void test_an_all_clear_played_back_does_not_end_a_later_event() {
  TestSite site;
  const Node node = upstreamNode(site);
  floodTo(site, node, AlertLevel::Warning);
  waterRecedes(site);
  issueAllClearAndWait(site);
  const std::vector<Frame> recorded = site.framesOnAir(PacketKind::AllClear);
  TEST_ASSERT_FALSE(recorded.empty());

  floodTo(site, node, AlertLevel::Evacuate);

  // Someone plays the recorded all clears back, for minutes on end.
  for (uint64_t elapsed = 0; elapsed < 3 * node.config.alertRefreshIntervalMs; elapsed += kStepMs) {
    for (const Frame& frame : recorded) site.air.inject(frame);
    site.runMs(kStepMs);
    TEST_ASSERT_TRUE(site.siren.isOn());
  }
  ASSERT_ALERT_LEVEL(AlertLevel::Evacuate, readingsFrom(site, node).back().reading.alertLevel);
}

// --- Wadi nodes drop their alert level ---

static void test_a_wadi_node_returns_to_no_alert_and_stops_sending_its_alert_again() {
  TestSite site;
  const Node node = upstreamNode(site);
  floodTo(site, node, AlertLevel::Evacuate);
  waterRecedes(site);
  // Until the all clear the node holds Evacuate and says so every minute.
  ASSERT_ALERT_LEVEL(AlertLevel::Evacuate, readingsFrom(site, node).back().reading.alertLevel);

  issueAllClearAndWait(site);

  // The node says at once that it holds no alert level any more.
  ASSERT_ALERT_LEVEL(AlertLevel::None, readingsFrom(site, node).back().reading.alertLevel);
  const size_t alertsAfterTheAllClear = alertsFrom(site, node).size();
  site.runMs(10 * node.config.alertRefreshIntervalMs);
  TEST_ASSERT_EQUAL_UINT(alertsAfterTheAllClear, alertsFrom(site, node).size());
  ASSERT_ALERT_LEVEL(AlertLevel::None, readingsFrom(site, node).back().reading.alertLevel);
}

static void test_the_transmissions_an_alert_still_had_to_come_are_not_made_after_the_all_clear() {
  TestSite site;
  // The node spaces the transmissions of an alert as far apart as it may.
  site.wadiNodeRandom.setDraws(std::vector<uint32_t>(64, wadiwatch::testing::FakeRandom::kHighest));
  const Node node = upstreamNode(site);
  floodTo(site, node, AlertLevel::Warning);
  waterRecedes(site);

  issueAllClearAndWait(site);

  // The alert the gateway answered went out once: the node heard the all clear
  // before the second transmission was due.
  const Frame lastAlert = site.framesOnAir(PacketKind::Alert).back();
  TEST_ASSERT_EQUAL_UINT(1, site.timesTransmitted(lastAlert));
}

static void test_after_the_all_clear_a_wadi_node_returns_to_dry_mode() {
  TestSite site;
  const Node node = upstreamNode(site);
  floodTo(site, node, AlertLevel::Warning);
  waterRecedes(site);
  const uint64_t hourMs = 60 * kMinuteMs;
  // A node at Warning stays in storm mode however long the bed has been dry.
  site.runMs(2ull * node.config.mode.stormQuietMs);
  TEST_ASSERT_TRUE(measurementsWithin(site, node, hourMs) >=
                   hourMs / node.config.mode.stormMeasureIntervalMs - 1);

  issueAllClearAndWait(site);
  // The quiet period starts at the all clear.
  site.runMs(node.config.mode.stormQuietMs);

  const size_t readingsBefore = readingsFrom(site, node).size();
  const size_t measurements = measurementsWithin(site, node, hourMs);
  TEST_ASSERT_TRUE(measurements <= hourMs / node.config.mode.dryMeasureIntervalMs + 1);
  TEST_ASSERT_TRUE(measurements >= hourMs / node.config.mode.dryMeasureIntervalMs - 1);
  // Dry mode sends no readings, and the node sleeps between its measurements.
  TEST_ASSERT_EQUAL_UINT(readingsBefore, readingsFrom(site, node).size());
  TEST_ASSERT_TRUE(site.wadiNodeSleep.isAsleep());
  TEST_ASSERT_FALSE(node.radio.isListening());
}

static void test_an_all_clear_for_a_wadi_covers_every_wadi_node_on_it() {
  TestSite site;
  const Node upstream = upstreamNode(site);
  const Node downstream = downstreamNode(site);
  floodTo(site, upstream, AlertLevel::Warning);
  // Warning downstream with Warning upstream: Evacuate by upstream confirmation.
  downstream.levelSensor.setWaterLevelCm(levelWithin(downstream, AlertLevel::Warning));
  for (uint64_t waited = 0; site.latestAlertOnAir(downstream.config.id) != AlertLevel::Evacuate;
       waited += kStepMs) {
    TEST_ASSERT_TRUE(waited < downstream.config.mode.dryMeasureIntervalMs + 2 * kMinuteMs);
    site.runMs(kStepMs);
  }
  waterRecedes(site);
  ASSERT_SIREN_SOUND(SirenSound::Continuous, site.listenToSiren());

  issueAllClearAndWait(site);

  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
  for (const Node& node : {upstream, downstream}) {
    ASSERT_ALERT_LEVEL(AlertLevel::None, readingsFrom(site, node).back().reading.alertLevel);
    const size_t alerts = alertsFrom(site, node).size();
    site.runMs(5 * node.config.alertRefreshIntervalMs);
    TEST_ASSERT_EQUAL_UINT(alerts, alertsFrom(site, node).size());
  }
  TEST_ASSERT_FALSE(sirenSoundsWithin(site, 5 * kMinuteMs));
}

static void test_after_the_all_clear_a_warning_heard_from_upstream_before_it_confirms_nothing() {
  TestSite site;
  const Node upstream = upstreamNode(site);
  const Node downstream = downstreamNode(site);
  floodTo(site, upstream, AlertLevel::Warning);
  downstream.levelSensor.setWaterLevelCm(levelWithin(downstream, AlertLevel::Warning));
  for (uint64_t waited = 0; site.latestAlertOnAir(downstream.config.id) != AlertLevel::Evacuate;
       waited += kStepMs) {
    TEST_ASSERT_TRUE(waited < downstream.config.mode.dryMeasureIntervalMs + 2 * kMinuteMs);
    site.runMs(kStepMs);
  }
  waterRecedes(site);
  issueAllClearAndWait(site);
  const size_t alertsBefore = alertsFrom(site, downstream).size();

  // A second flood reaches the downstream node only: Warning there, a dry bed upstream.
  downstream.levelSensor.setWaterLevelCm(levelWithin(downstream, AlertLevel::Warning));
  site.runMs(5 * downstream.config.alertRefreshIntervalMs);

  // The Warning the upstream node sent in the event that is over does not make it Evacuate.
  const std::vector<Packet> alerts = alertsFrom(site, downstream);
  TEST_ASSERT_TRUE(alerts.size() > alertsBefore);
  for (size_t i = alertsBefore; i < alerts.size(); ++i) {
    ASSERT_ALERT_LEVEL(AlertLevel::Warning, alerts[i].alert.alertLevel);
  }
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());
}

static void test_a_wadi_node_whose_water_still_calls_for_an_alert_raises_it_again_as_a_new_event() {
  TestSite site;
  const Node node = upstreamNode(site);
  floodTo(site, node, AlertLevel::Warning);
  letSmsGoOut(site);
  const Recipient& official = recipient(site, 0);
  TEST_ASSERT_TRUE(official.group == RecipientGroup::Official);
  TEST_ASSERT_EQUAL_UINT(1, textsSentTo(site, official).size());

  // The backend's rule would have refused this all clear: the water is still high.
  // The devices do not depend on that rule to stay safe.
  issueAllClearAndWait(site);
  letSmsGoOut(site);

  // The node dropped its alert, measured the same water and raised Warning again.
  ASSERT_ALERT_LEVEL(AlertLevel::Warning, readingsFrom(site, node).back().reading.alertLevel);
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());
  // For the gateway that is a new event: the alert SMS, the closing SMS, the alert SMS again.
  const std::vector<std::string> texts = textsSentTo(site, official);
  TEST_ASSERT_EQUAL_UINT(3, texts.size());
  TEST_ASSERT_TRUE(texts[0] == texts[2]);
  TEST_ASSERT_TRUE(texts[1] == closingText(official, site.config.gateway.wadis[0]));
  // And the gateway does not answer the new alert with the old all clear.
  const size_t allClears = allClearsFor(site, node).size();
  site.runMs(5 * node.config.alertRefreshIntervalMs);
  TEST_ASSERT_EQUAL_UINT(allClears, allClearsFor(site, node).size());
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());
}

// --- An alert that raises the alert level is never taken for the old one ---

static void test_a_wadi_node_marks_the_alert_it_sends_again_as_a_refresh_and_the_one_that_raises_as_not() {
  TestSite site;
  const Node node = upstreamNode(site);
  floodTo(site, node, AlertLevel::Warning);
  site.runMs(3 * node.config.alertRefreshIntervalMs);
  floodTo(site, node, AlertLevel::Evacuate);
  site.runMs(3 * node.config.alertRefreshIntervalMs);

  const std::vector<Packet> alerts = alertsFrom(site, node);
  TEST_ASSERT_TRUE(alerts.size() >= 6);
  AlertLevel announced = AlertLevel::None;
  size_t raises = 0;
  for (const Packet& alert : alerts) {
    const bool raisesTheLevel = alert.alert.alertLevel > announced;
    TEST_ASSERT_EQUAL(!raisesTheLevel, alert.alert.refresh);
    if (raisesTheLevel) ++raises;
    announced = alert.alert.alertLevel;
  }
  TEST_ASSERT_EQUAL_UINT(2, raises);
}

static void test_a_wadi_node_that_restarts_into_a_flood_after_the_all_clear_is_believed_at_once() {
  TestSite site;
  const Node node = upstreamNode(site);
  floodTo(site, node, AlertLevel::Warning);
  letSmsGoOut(site);
  // The node loses power with the wadi in flood, and the officer ends the event.
  site.unplugWadiNode();
  issueAllClearAndWait(site);
  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
  const size_t allClears = allClearsFor(site, node).size();
  TEST_ASSERT_EQUAL_UINT(1, allClears);

  // Power returns while the gateway still waits to hear that the node has heard. The
  // node measures the flood and raises Warning: an alert that raises, not a refresh.
  site.restartWadiNode();
  const size_t alertsBefore = alertsFrom(site, node).size();
  for (uint64_t waited = 0; alertsFrom(site, node).size() == alertsBefore; waited += kStepMs) {
    TEST_ASSERT_TRUE(waited < kMinuteMs);
    site.runMs(kStepMs);
  }

  // From its first transmission on the siren sounds, without a break, and the
  // gateway answers with no all clear.
  for (uint64_t elapsed = 0; elapsed < 3 * node.config.alertRefreshIntervalMs; elapsed += kStepMs) {
    TEST_ASSERT_TRUE(site.siren.isStrobeOn());
    site.runMs(kStepMs);
  }
  TEST_ASSERT_EQUAL_UINT(allClears, allClearsFor(site, node).size());
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());
  // It is a new event: the people are told again.
  letSmsGoOut(site);
  const std::vector<std::string> texts = textsSentTo(site, recipient(site, 0));
  TEST_ASSERT_EQUAL_UINT(3, texts.size());
  TEST_ASSERT_TRUE(texts[2] == texts[0]);
}

static void test_an_all_clear_still_held_for_a_wadi_node_is_dropped_when_that_node_raises_its_alert_level() {
  TestSite site;
  const Node node = upstreamNode(site);
  floodTo(site, node, AlertLevel::Warning);
  letSmsGoOut(site);
  // The scenario issues the command right after a transmission of the node's alert,
  // so that the node's next measurement comes before its next refresh.
  const size_t alertsBefore = alertsFrom(site, node).size();
  while (alertsFrom(site, node).size() == alertsBefore) site.runMs(kStepMs);

  // An all clear the backend's rule would have refused, and the flood grows before
  // the gateway has transmitted it.
  site.issueAllClear();
  node.levelSensor.setWaterLevelCm(levelWithin(node, AlertLevel::Evacuate));
  site.runMs(site.allClearSettleMs());
  letSmsGoOut(site);

  // No all clear was ever transmitted for that node, and the siren went from the
  // Warning pattern to continuous without falling silent.
  TEST_ASSERT_EQUAL_UINT(0, allClearsFor(site, node).size());
  ASSERT_SIREN_SOUND(SirenSound::Continuous, site.listenToSiren());
  ASSERT_ALERT_LEVEL(AlertLevel::Evacuate, readingsFrom(site, node).back().reading.alertLevel);
  // The Evacuate is told as the start of a new event.
  const std::vector<std::string> texts = textsSentTo(site, recipient(site, 0));
  TEST_ASSERT_EQUAL_UINT(3, texts.size());
  TEST_ASSERT_TRUE(texts[1] == closingText(recipient(site, 0), site.config.gateway.wadis[0]));
}

// --- A wadi node that does not hear the first all clear ---

static void test_a_wadi_node_that_missed_the_all_clear_is_given_it_again_and_no_new_event_starts() {
  TestSite site;
  const Node node = upstreamNode(site);
  floodTo(site, node, AlertLevel::Warning);
  letSmsGoOut(site);
  waterRecedes(site);
  const size_t smsBefore = site.modem.sent().size();
  // Every transmission of the gateway's first all clear packet is lost on the air.
  site.air.dropNextFrom(site.gatewayRadio, site.config.gateway.allClearRepeat.transmissions);

  issueAllClearAndWait(site);
  site.runMs(2 * node.config.alertRefreshIntervalMs);
  letSmsGoOut(site);

  // The node sent its alert again, the gateway answered again, and that one arrived.
  TEST_ASSERT_EQUAL_UINT(2, allClearsFor(site, node).size());
  ASSERT_ALERT_LEVEL(AlertLevel::None, readingsFrom(site, node).back().reading.alertLevel);
  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
  // The alert the node sent meanwhile was the old one: only closing SMS went out.
  TEST_ASSERT_EQUAL_UINT(smsBefore + closingSmsCount(site), site.modem.sent().size());
  TEST_ASSERT_TRUE(closingSmsCount(site) > 0);
  // Nothing more follows.
  const size_t alerts = alertsFrom(site, node).size();
  site.runMs(5 * node.config.alertRefreshIntervalMs);
  TEST_ASSERT_EQUAL_UINT(alerts, alertsFrom(site, node).size());
  TEST_ASSERT_EQUAL_UINT(2, allClearsFor(site, node).size());
}

static void test_a_wadi_node_that_never_hears_is_answered_a_limited_number_of_times_then_starts_a_new_event() {
  TestSite site;
  const Node node = upstreamNode(site);
  floodTo(site, node, AlertLevel::Warning);
  letSmsGoOut(site);
  waterRecedes(site);
  const Recipient& official = recipient(site, 0);
  // Nothing the gateway transmits arrives any more: a wadi node with a dead receiver
  // would see the same.
  site.air.dropNextFrom(site.gatewayRadio, 100000);

  site.issueAllClear();
  site.runMs((site.config.gateway.allClearMaxAnswers + 3ull) * node.config.alertRefreshIntervalMs);
  letSmsGoOut(site);

  // The gateway did not go on answering for ever,
  TEST_ASSERT_EQUAL_UINT(site.config.gateway.allClearMaxAnswers,
                         allClearsFor(site, node).size());
  // and then took the alert the node keeps sending for what the siren node takes it
  // for: an alert in force. The people are told again.
  const std::vector<std::string> texts = textsSentTo(site, official);
  TEST_ASSERT_EQUAL_UINT(3, texts.size());
  TEST_ASSERT_TRUE(texts[1] == closingText(official, site.config.gateway.wadis[0]));
  TEST_ASSERT_TRUE(texts[2] == texts[0]);
}

// The gateway can miss the alert that raises a node's alert level and hear only the
// alert refresh that follows. A refresh at a higher alert level than the all clear
// ended is not the old alert.
static void test_an_alert_refresh_above_the_alert_level_the_all_clear_ended_starts_a_new_event() {
  TestSite site;
  const Node node = upstreamNode(site);
  const Recipient& official = recipient(site, 0);
  const WadiConfig& wadi = site.config.gateway.wadis[0];
  floodTo(site, node, AlertLevel::Warning);
  letSmsGoOut(site);
  waterRecedes(site);
  // The node hears nothing the gateway transmits, so it stays at Warning and goes on
  // sending its alert refresh, which the gateway answers.
  site.air.dropNextFrom(site.gatewayRadio, 100000);
  site.issueAllClear();
  site.runMs(node.config.alertRefreshIntervalMs + kStepMs);
  TEST_ASSERT_TRUE(allClearsFor(site, node).size() >= 1);

  // The water comes back and reaches Evacuate. Interference swallows what the node
  // transmits until the alert that raises its level has had all its transmissions.
  site.air.dropNextFrom(node.radio, 100000);
  node.levelSensor.setWaterLevelCm(levelWithin(node, AlertLevel::Evacuate));
  for (uint64_t waited = 0; site.latestAlertOnAir(node.config.id) != AlertLevel::Evacuate;
       waited += kStepMs) {
    TEST_ASSERT_TRUE(waited < 2 * node.config.mode.stormMeasureIntervalMs);
    site.runMs(kStepMs);
  }
  site.runMs(static_cast<uint64_t>(node.config.alertRepeat.transmissions) *
             node.config.alertRepeat.maxSpacingMs);
  site.air.stopDroppingFrom(node.radio);
  const size_t allClearsBefore = allClearsFor(site, node).size();
  TEST_ASSERT_TRUE_MESSAGE(allClearsBefore + 2 <= site.config.gateway.allClearMaxAnswers,
                           "this scenario needs the gateway to have answers left");

  // The next thing the gateway hears from the node is an alert refresh at Evacuate.
  site.runMs(2 * node.config.alertRefreshIntervalMs);
  letSmsGoOut(site);

  // It is not answered with an all clear,
  TEST_ASSERT_EQUAL_UINT(allClearsBefore, allClearsFor(site, node).size());
  // and the people are told of Evacuate, as the start of a new event.
  const std::vector<std::string> texts = textsSentTo(site, official);
  TEST_ASSERT_EQUAL_UINT(3, texts.size());
  TEST_ASSERT_TRUE(texts[0] == alertText(official, wadi, AlertLevel::Warning));
  TEST_ASSERT_TRUE(texts[1] == closingText(official, wadi));
  TEST_ASSERT_TRUE(texts[2] == alertText(official, wadi, AlertLevel::Evacuate));
}

// A gateway that restarted during the event has not heard of the alert its command
// ends. The alert refresh it hears while it still holds the all clear is that alert.
static void test_a_gateway_restarted_during_the_event_answers_the_alert_refresh_it_hears_while_holding_the_all_clear() {
  TestSite site;
  const Node node = upstreamNode(site);
  const Recipient& official = recipient(site, 0);
  floodTo(site, node, AlertLevel::Warning);
  letSmsGoOut(site);
  waterRecedes(site);
  // The gateway restarts just after an alert refresh, and the command arrives before
  // the next one.
  const size_t alertsBefore = alertsFrom(site, node).size();
  while (alertsFrom(site, node).size() == alertsBefore) site.runMs(kStepMs);
  site.runMs(static_cast<uint64_t>(node.config.alertRepeat.transmissions) *
             node.config.alertRepeat.maxSpacingMs);
  site.restartGateway();

  issueAllClearAndWait(site);
  letSmsGoOut(site);

  // The node heard the all clear in the answer window of its alert refresh.
  TEST_ASSERT_EQUAL_UINT(1, allClearsFor(site, node).size());
  ASSERT_ALERT_LEVEL(AlertLevel::None, readingsFrom(site, node).back().reading.alertLevel);
  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
  // No new event was started for that refresh: nobody was told of Warning twice.
  const std::vector<std::string> texts = textsSentTo(site, official);
  TEST_ASSERT_EQUAL_UINT(1, texts.size());
}

static void test_the_gateway_s_airtime_stays_under_the_duty_cycle_limit_even_when_no_wadi_node_hears() {
  TestSite site;
  const Node nodes[] = {upstreamNode(site), downstreamNode(site)};
  for (const Node& node : nodes) floodTo(site, node, AlertLevel::Warning);
  waterRecedes(site);
  // Both wadi nodes are deaf, so the gateway answers each as often as it ever will.
  site.air.dropNextFrom(site.gatewayRadio, 100000);
  const wadiwatch::RadioConfig& radio = site.config.radio;

  site.issueAllClear();
  site.runMs(radio.dutyCyclePeriodMs);

  uint64_t onAirUs = 0;
  for (const Frame& frame : site.gatewayRadio.sent()) {
    onAirUs += wadiwatch::loraTimeOnAirUs(radio, frame.size());
  }
  const double allowedUs =
      static_cast<double>(radio.dutyCyclePeriodMs) * 1000.0 * radio.dutyCycleLimitPercent / 100.0;
  TEST_ASSERT_TRUE(static_cast<double>(onAirUs) < allowedUs);
  // The gateway did transmit: every answer it owed, each packet all its times.
  TEST_ASSERT_EQUAL_UINT(2u * site.config.gateway.allClearMaxAnswers *
                             site.config.gateway.allClearRepeat.transmissions,
                         site.gatewayRadio.sent().size());
}

static void test_a_wadi_node_at_watch_in_dry_mode_hears_the_all_clear_after_its_heartbeat() {
  TestSite site;
  const Node node = upstreamNode(site);
  floodTo(site, node, AlertLevel::Watch);
  waterRecedes(site);
  // Watch does not hold the node in storm mode: after the quiet period it measures
  // every fifteen minutes and sends nothing but its hourly heartbeat.
  site.runMs(node.config.mode.stormQuietMs + 2 * node.config.mode.dryMeasureIntervalMs);
  ASSERT_ALERT_LEVEL(AlertLevel::Watch, readingsFrom(site, node).back().reading.alertLevel);
  const size_t readingsBefore = readingsFrom(site, node).size();

  site.issueAllClear();
  site.runMs(node.config.heartbeatIntervalMs + site.allClearSettleMs());

  // The gateway answered the heartbeat, and the node said that it holds no alert level.
  TEST_ASSERT_EQUAL_UINT(readingsBefore + 1, readingsFrom(site, node).size());
  ASSERT_ALERT_LEVEL(AlertLevel::None, readingsFrom(site, node).back().reading.alertLevel);

  // So the next rise to Watch is announced, which a node still at Watch would not do.
  const size_t watchAlertsBefore = alertsFrom(site, node).size();
  node.levelSensor.setWaterLevelCm(levelWithin(node, AlertLevel::Watch));
  site.runMs(node.config.mode.dryMeasureIntervalMs + kMinuteMs);
  TEST_ASSERT_EQUAL_UINT(watchAlertsBefore + 1, alertsFrom(site, node).size());
  ASSERT_ALERT_LEVEL(AlertLevel::Watch, alertsFrom(site, node).back().alert.alertLevel);
}

// --- What the answer window costs ---

static void test_a_wadi_node_with_no_alert_never_switches_its_receiver_on() {
  TestSite site;
  const Node node = upstreamNode(site);
  // A day with rain and a level below Watch: storm mode, readings, heartbeats, no alert.
  site.rainGauge.tip();

  const uint64_t steps = 3 * node.config.heartbeatIntervalMs / kStepMs;
  TEST_ASSERT_EQUAL_UINT64(0, stepsListening(site, node, steps));
  TEST_ASSERT_FALSE(readingsFrom(site, node).empty());
  TEST_ASSERT_FALSE(site.packetsOnAir(PacketKind::Heartbeat, node.config.id).empty());
}

static void test_a_wadi_node_in_alert_listens_only_for_the_answer_window_after_each_transmission() {
  TestSite site;
  const Node node = upstreamNode(site);
  floodTo(site, node, AlertLevel::Evacuate);
  // Past the transmissions of the first alert.
  site.runMs(node.config.alertRefreshIntervalMs / 2);

  size_t framesBefore = node.radio.sent().size();
  uint64_t lastSentAtMs = 0;
  bool sent = false;
  for (uint64_t elapsed = 0; elapsed < 5 * node.config.alertRefreshIntervalMs; elapsed += kStepMs) {
    site.runMs(kStepMs);
    if (node.radio.sent().size() != framesBefore) {
      framesBefore = node.radio.sent().size();
      lastSentAtMs = site.clock.nowMs();
      sent = true;
    }
    const bool windowOpen = sent && site.clock.nowMs() - lastSentAtMs < node.config.answerWindowMs;
    TEST_ASSERT_EQUAL(windowOpen, node.radio.isListening());
  }
  TEST_ASSERT_TRUE(sent);
}

// --- The closing SMS ---

static void test_a_closing_sms_goes_once_to_every_recipient_who_received_an_alert_for_the_event() {
  TestSite site;
  floodTo(site, upstreamNode(site), AlertLevel::Warning);
  letSmsGoOut(site);
  waterRecedes(site);
  const size_t alertSmsCount = site.modem.sent().size();
  TEST_ASSERT_TRUE(alertSmsCount > 0);

  issueAllClearAndWait(site);
  letSmsGoOut(site);

  size_t told = 0;
  size_t notTold = 0;
  for (size_t slot = 0; slot < wadiwatch::kMaxRecipients; ++slot) {
    const Recipient& one = recipient(site, slot);
    if (!isRegistered(one)) continue;
    const std::vector<std::string> texts = textsSentTo(site, one);
    if (toldOfWarning(site, one)) {
      ++told;
      // The alert, then the closing SMS: in the recipient's language, naming the wadi.
      TEST_ASSERT_EQUAL_UINT(2, texts.size());
      TEST_ASSERT_TRUE(texts[1] == closingText(one, site.config.gateway.wadis[0]));
      TEST_ASSERT_TRUE(contains(texts[1], wadiNameIn(site.config.gateway.wadis[0], one.language)));
      TEST_ASSERT_TRUE(contains(texts[1], wadiwatch::allClearName(one.language)));
    } else {
      // Whoever was never told of the event is not told of its end.
      ++notTold;
      TEST_ASSERT_EQUAL_UINT(0, texts.size());
    }
  }
  // The committed list has recipients of both sorts.
  TEST_ASSERT_TRUE(told > 0);
  TEST_ASSERT_TRUE(notTold > 0);

  // Once: nothing more goes out however long the site runs on.
  const size_t smsCount = site.modem.sent().size();
  site.runMs(site.config.wadiNode.heartbeatIntervalMs);
  TEST_ASSERT_EQUAL_UINT(smsCount, site.modem.sent().size());
}

static void test_a_recipient_whose_alert_sms_never_went_out_gets_no_closing_sms() {
  TestSite site;
  // No mobile network for the whole event.
  site.modem.startOutage();
  floodTo(site, upstreamNode(site), AlertLevel::Warning);
  letSmsGoOut(site);
  waterRecedes(site);

  issueAllClearAndWait(site);
  site.modem.endOutage();
  site.runMs(site.config.gateway.smsRetryIntervalMs);
  letSmsGoOut(site);

  // The alert SMS that were still waiting are dropped, and there is nothing to close.
  TEST_ASSERT_EQUAL_UINT(0, site.modem.sent().size());
}

static void test_with_gsm_down_the_all_clear_still_goes_out_and_the_closing_sms_follow_once_gsm_is_back() {
  TestSite site;
  floodTo(site, upstreamNode(site), AlertLevel::Warning);
  letSmsGoOut(site);
  waterRecedes(site);
  const size_t alertSmsCount = site.modem.sent().size();
  site.modem.startOutage();

  issueAllClearAndWait(site);
  site.runMs(4ull * site.config.gateway.smsRetryIntervalMs);

  // The radio does not wait for the modem: the siren is silent and the node at no alert.
  TEST_ASSERT_EQUAL_UINT(2, allClearsOnAir(site).size());
  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
  TEST_ASSERT_EQUAL_UINT(alertSmsCount, site.modem.sent().size());
  TEST_ASSERT_FALSE(site.modem.refused().empty());

  site.modem.endOutage();
  site.runMs(site.config.gateway.smsRetryIntervalMs);
  letSmsGoOut(site);

  // Each closing SMS once, to the same people as the alert.
  TEST_ASSERT_EQUAL_UINT(alertSmsCount, closingSmsCount(site));
  TEST_ASSERT_EQUAL_UINT(2 * alertSmsCount, site.modem.sent().size());
  for (size_t slot = 0; slot < wadiwatch::kMaxRecipients; ++slot) {
    if (!isRegistered(recipient(site, slot))) continue;
    TEST_ASSERT_TRUE(closingSmsCountTo(site, recipient(site, slot)) <= 1);
  }
  site.runMs(site.config.wadiNode.heartbeatIntervalMs);
  TEST_ASSERT_EQUAL_UINT(2 * alertSmsCount, site.modem.sent().size());
}

// --- The backend is told ---

static void test_the_all_clear_is_reported_to_the_backend_once_for_each_wadi_node() {
  TestSite site;
  floodTo(site, upstreamNode(site), AlertLevel::Warning);
  waterRecedes(site);

  const uint32_t command = issueAllClearAndWait(site);
  site.runMs(5 * kMinuteMs);

  // One message for each packet, however many times the packet was transmitted.
  const std::vector<std::string> messages = allClearMessages(site);
  TEST_ASSERT_EQUAL_UINT(2, messages.size());
  for (const Node& node : {upstreamNode(site), downstreamNode(site)}) {
    size_t naming = 0;
    for (const std::string& message : messages) {
      if (contains(message, "\"wadi_node\":" + std::to_string(node.config.id) + ",")) ++naming;
    }
    TEST_ASSERT_EQUAL_UINT(1, naming);
  }
  for (const std::string& message : messages) {
    TEST_ASSERT_TRUE(contains(message, "\"node\":" + std::to_string(site.config.gateway.id) + ","));
    TEST_ASSERT_TRUE(contains(message, "\"command\":" + std::to_string(command) + "}"));
  }
  // The wadi node's own word that it holds no alert level follows its all clear.
  const std::string noneFromTheNode =
      "\"kind\":\"reading\",\"node\":" + std::to_string(site.config.wadiNode.id) + ",";
  bool confirmed = false;
  bool sawItsAllClear = false;
  for (const std::string& message : site.uplink.published()) {
    if (contains(message, "\"kind\":\"all_clear\"") &&
        contains(message, "\"wadi_node\":" + std::to_string(site.config.wadiNode.id) + ",")) {
      sawItsAllClear = true;
    }
    if (sawItsAllClear && contains(message, noneFromTheNode) &&
        contains(message, "\"alert_level\":\"none\"")) {
      confirmed = true;
    }
  }
  TEST_ASSERT_TRUE(confirmed);
}

static void test_with_the_uplink_down_after_the_command_the_all_clear_goes_out_and_is_reported_later() {
  TestSite site;
  floodTo(site, upstreamNode(site), AlertLevel::Warning);
  waterRecedes(site);

  site.issueAllClear();
  // The gateway takes the command in; then the uplink goes down.
  site.runMs(kStepMs);
  site.uplink.startOutage();
  site.runMs(site.allClearSettleMs());

  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
  TEST_ASSERT_EQUAL_UINT(2, allClearsOnAir(site).size());
  TEST_ASSERT_EQUAL_UINT(0, allClearMessages(site).size());

  site.uplink.endOutage();
  site.runMs(site.config.gateway.uplinkRetryIntervalMs + 5 * kMinuteMs);

  TEST_ASSERT_EQUAL_UINT(2, allClearMessages(site).size());
}

// --- Commands the gateway does not act on ---

static void test_a_command_that_waited_too_long_at_the_broker_is_not_acted_on() {
  TestSite site;
  const Node node = upstreamNode(site);
  floodTo(site, node, AlertLevel::Warning);
  letSmsGoOut(site);
  const size_t smsBefore = site.modem.sent().size();
  // The uplink is down when the officer issues the all clear. The command waits at
  // the broker, and the connection comes back only when it is no longer fresh.
  site.uplink.startOutage();
  site.issueAllClear();
  site.runMs((site.config.gateway.commandMaxAgeS + 60ull) * 1000);
  site.uplink.endOutage();

  site.runMs(site.allClearSettleMs());
  letSmsGoOut(site);

  // The event is whatever it is by now, and it is not ended by an old decision.
  TEST_ASSERT_EQUAL_UINT(0, site.packetsOnAir(PacketKind::AllClear).size());
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());
  TEST_ASSERT_EQUAL_UINT(smsBefore, site.modem.sent().size());
  ASSERT_ALERT_LEVEL(AlertLevel::Warning, readingsFrom(site, node).back().reading.alertLevel);
}

static void test_a_command_that_waited_a_short_while_at_the_broker_is_acted_on() {
  TestSite site;
  floodTo(site, upstreamNode(site), AlertLevel::Warning);
  waterRecedes(site);
  site.uplink.startOutage();
  site.issueAllClear();
  site.runMs(site.config.gateway.commandMaxAgeS * 1000ull / 2);
  site.uplink.endOutage();

  site.runMs(site.allClearSettleMs());

  TEST_ASSERT_EQUAL_UINT(2, allClearsOnAir(site).size());
  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
}

static void test_a_command_is_not_acted_on_while_the_gateway_does_not_know_the_time() {
  TestSite site;
  floodTo(site, upstreamNode(site), AlertLevel::Warning);
  waterRecedes(site);
  // The gateway cannot tell how old the command is.
  site.wallClock.forgetTime();

  issueAllClearAndWait(site);

  TEST_ASSERT_EQUAL_UINT(0, site.packetsOnAir(PacketKind::AllClear).size());
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());

  // The officer issues it again once the gateway has the time.
  site.wallClock.learnTime();
  issueAllClearAndWait(site);
  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
}

static void test_a_command_dated_in_the_future_is_not_acted_on() {
  TestSite site;
  floodTo(site, upstreamNode(site), AlertLevel::Warning);
  waterRecedes(site);
  const uint32_t tooFarAhead =
      site.wallClock.unixSeconds() + 2 * site.config.gateway.commandMaxAgeS;

  site.uplink.deliver(
      TestSite::allClearCommand(1, tooFarAhead, site.config.gateway.wadis[0].id));
  site.runMs(site.allClearSettleMs());

  TEST_ASSERT_EQUAL_UINT(0, site.packetsOnAir(PacketKind::AllClear).size());
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());
}

static void test_a_command_delivered_twice_is_acted_on_once() {
  TestSite site;
  floodTo(site, upstreamNode(site), AlertLevel::Warning);
  letSmsGoOut(site);
  waterRecedes(site);
  const std::string command = TestSite::allClearCommand(7, site.wallClock.unixSeconds(),
                                                        site.config.gateway.wadis[0].id);

  site.uplink.deliver(command);
  site.runMs(site.allClearSettleMs());
  // The broker delivers at least once.
  site.uplink.deliver(command);
  site.runMs(site.allClearSettleMs());
  letSmsGoOut(site);

  TEST_ASSERT_EQUAL_UINT(2, allClearsOnAir(site).size());
  for (size_t slot = 0; slot < wadiwatch::kMaxRecipients; ++slot) {
    if (!isRegistered(recipient(site, slot))) continue;
    TEST_ASSERT_TRUE(closingSmsCountTo(site, recipient(site, slot)) <= 1);
  }
  TEST_ASSERT_TRUE(closingSmsCount(site) > 0);
}

static void test_a_later_command_with_the_number_of_an_earlier_one_is_acted_on() {
  TestSite site;
  const Node node = upstreamNode(site);
  const WadiId wadi = site.config.gateway.wadis[0].id;
  floodTo(site, node, AlertLevel::Warning);
  waterRecedes(site);
  site.uplink.deliver(TestSite::allClearCommand(1, site.wallClock.unixSeconds(), wadi));
  site.runMs(site.allClearSettleMs());
  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());

  // A second flood, and a backend whose database was started afresh in between: it
  // numbers its commands from one again.
  floodTo(site, node, AlertLevel::Warning);
  waterRecedes(site);
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());
  site.uplink.deliver(TestSite::allClearCommand(1, site.wallClock.unixSeconds(), wadi));
  site.runMs(site.allClearSettleMs());

  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
  ASSERT_ALERT_LEVEL(AlertLevel::None, readingsFrom(site, node).back().reading.alertLevel);
}

static void test_a_command_the_gateway_cannot_make_sense_of_changes_nothing() {
  TestSite site;
  floodTo(site, upstreamNode(site), AlertLevel::Warning);
  letSmsGoOut(site);
  const size_t smsBefore = site.modem.sent().size();
  const uint32_t now = site.wallClock.unixSeconds();
  const WadiId wadi = site.config.gateway.wadis[0].id;
  const WadiId unknownWadi = static_cast<WadiId>(wadi + 100);
  const std::string wellFormed = TestSite::allClearCommand(3, now, wadi);

  site.uplink.deliver("");
  site.uplink.deliver("all clear");
  site.uplink.deliver("{}");
  site.uplink.deliver(wellFormed.substr(0, wellFormed.size() - 1));
  site.uplink.deliver(TestSite::allClearCommand(3, now, unknownWadi));
  site.uplink.deliver(std::string(2 * wadiwatch::kMaxDownlinkCommandLength, ' ') + wellFormed);
  site.runMs(site.allClearSettleMs());
  letSmsGoOut(site);

  TEST_ASSERT_EQUAL_UINT(0, site.packetsOnAir(PacketKind::AllClear).size());
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());
  TEST_ASSERT_EQUAL_UINT(smsBefore, site.modem.sent().size());

  // Control: the well-formed command behind them is acted on.
  site.uplink.deliver(TestSite::allClearCommand(3, site.wallClock.unixSeconds(), wadi));
  site.runMs(site.allClearSettleMs());
  TEST_ASSERT_EQUAL_UINT(2, allClearsOnAir(site).size());
}

// --- The command, read on its own ---

static void test_the_command_of_the_contract_is_read() {
  const std::string text = TestSite::allClearCommand(4000000000u, 1791594000u, 3);
  AllClearCommand command = AllClearCommand();

  TEST_ASSERT_TRUE(parseAllClearCommand(text.data(), text.size(), command));

  TEST_ASSERT_EQUAL_UINT32(4000000000u, command.command);
  TEST_ASSERT_EQUAL_UINT32(1791594000u, command.issuedAt);
  TEST_ASSERT_EQUAL_UINT8(3, command.wadi);

  // Field order and white space do not matter.
  TEST_ASSERT_TRUE(parses(" { \"wadi\" : 3 , \"ts\":1791594000,\n\"command\":12,\"kind\":\"all_clear\",\"v\":1 }\n"));
}

static void test_anything_else_is_refused_whole() {
  const char* const refused[] = {
      // not an object, or not finished
      "",
      "[]",
      "null",
      "{\"v\":1,\"kind\":\"all_clear\",\"command\":12,\"ts\":1791594000,\"wadi\":3",
      "{\"v\":1,\"kind\":\"all_clear\",\"command\":12,\"ts\":1791594000,\"wadi\":3}}",
      "{\"v\":1,\"kind\":\"all_clear\",\"command\":12,\"ts\":1791594000,\"wadi\":3,}",
      // a field missing
      "{\"kind\":\"all_clear\",\"command\":12,\"ts\":1791594000,\"wadi\":3}",
      "{\"v\":1,\"command\":12,\"ts\":1791594000,\"wadi\":3}",
      "{\"v\":1,\"kind\":\"all_clear\",\"ts\":1791594000,\"wadi\":3}",
      "{\"v\":1,\"kind\":\"all_clear\",\"command\":12,\"wadi\":3}",
      "{\"v\":1,\"kind\":\"all_clear\",\"command\":12,\"ts\":1791594000}",
      // another version, another kind
      "{\"v\":2,\"kind\":\"all_clear\",\"command\":12,\"ts\":1791594000,\"wadi\":3}",
      "{\"v\":1,\"kind\":\"alert\",\"command\":12,\"ts\":1791594000,\"wadi\":3}",
      "{\"v\":1,\"kind\":\"all_clear_now\",\"command\":12,\"ts\":1791594000,\"wadi\":3}",
      // a field given twice, a field the contract does not list
      "{\"v\":1,\"kind\":\"all_clear\",\"command\":12,\"ts\":1791594000,\"wadi\":3,\"wadi\":4}",
      "{\"v\":1,\"kind\":\"all_clear\",\"command\":12,\"ts\":1791594000,\"wadi\":3,\"wadi_node\":257}",
      // values out of range or of the wrong type
      "{\"v\":1,\"kind\":\"all_clear\",\"command\":0,\"ts\":1791594000,\"wadi\":3}",
      "{\"v\":1,\"kind\":\"all_clear\",\"command\":4294967296,\"ts\":1791594000,\"wadi\":3}",
      "{\"v\":1,\"kind\":\"all_clear\",\"command\":12,\"ts\":0,\"wadi\":3}",
      "{\"v\":1,\"kind\":\"all_clear\",\"command\":12,\"ts\":1791594000,\"wadi\":0}",
      "{\"v\":1,\"kind\":\"all_clear\",\"command\":12,\"ts\":1791594000,\"wadi\":256}",
      "{\"v\":1,\"kind\":\"all_clear\",\"command\":-12,\"ts\":1791594000,\"wadi\":3}",
      "{\"v\":1,\"kind\":\"all_clear\",\"command\":12.5,\"ts\":1791594000,\"wadi\":3}",
      "{\"v\":1,\"kind\":\"all_clear\",\"command\":1e3,\"ts\":1791594000,\"wadi\":3}",
      "{\"v\":1,\"kind\":\"all_clear\",\"command\":012,\"ts\":1791594000,\"wadi\":3}",
      "{\"v\":1,\"kind\":\"all_clear\",\"command\":\"12\",\"ts\":1791594000,\"wadi\":3}",
      "{\"v\":1,\"kind\":\"all_clear\",\"command\":12,\"ts\":1791594000,\"wadi\":[3]}",
      "{\"v\":1,\"kind\":all_clear,\"command\":12,\"ts\":1791594000,\"wadi\":3}",
      "{\"v\":true,\"kind\":\"all_clear\",\"command\":12,\"ts\":1791594000,\"wadi\":3}",
  };
  for (const char* text : refused) {
    TEST_ASSERT_FALSE_MESSAGE(parses(text), text);
  }
  // A command longer than the contract allows, however well formed.
  const std::string padded =
      std::string(wadiwatch::kMaxDownlinkCommandLength, ' ') + TestSite::allClearCommand(1, 1, 1);
  TEST_ASSERT_FALSE(parses(padded));
  // A command that is refused leaves the caller's copy alone.
  AllClearCommand untouched = {77, 88, 9};
  const std::string bad = "{\"v\":1,\"kind\":\"all_clear\",\"command\":12,\"ts\":5}";
  TEST_ASSERT_FALSE(parseAllClearCommand(bad.data(), bad.size(), untouched));
  TEST_ASSERT_EQUAL_UINT32(77, untouched.command);
  TEST_ASSERT_EQUAL_UINT32(88, untouched.issuedAt);
  TEST_ASSERT_EQUAL_UINT8(9, untouched.wadi);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_an_accepted_all_clear_is_transmitted_as_a_signed_packet_for_every_wadi_node_of_the_wadi);
  RUN_TEST(test_without_a_command_the_gateway_never_transmits_an_all_clear);
  RUN_TEST(test_the_all_clear_for_one_wadi_leaves_the_event_at_another_wadi_alone);
  RUN_TEST(test_the_siren_stops_on_the_accepted_all_clear_and_stays_silent);
  RUN_TEST(test_the_siren_is_not_sounded_again_between_the_command_and_the_wadi_node_hearing_it);
  RUN_TEST(test_a_forged_all_clear_stops_neither_the_siren_nor_the_wadi_node);
  RUN_TEST(test_an_all_clear_played_back_does_not_end_a_later_event);
  RUN_TEST(test_a_wadi_node_returns_to_no_alert_and_stops_sending_its_alert_again);
  RUN_TEST(test_the_transmissions_an_alert_still_had_to_come_are_not_made_after_the_all_clear);
  RUN_TEST(test_after_the_all_clear_a_wadi_node_returns_to_dry_mode);
  RUN_TEST(test_an_all_clear_for_a_wadi_covers_every_wadi_node_on_it);
  RUN_TEST(test_after_the_all_clear_a_warning_heard_from_upstream_before_it_confirms_nothing);
  RUN_TEST(test_a_wadi_node_whose_water_still_calls_for_an_alert_raises_it_again_as_a_new_event);
  RUN_TEST(test_a_wadi_node_marks_the_alert_it_sends_again_as_a_refresh_and_the_one_that_raises_as_not);
  RUN_TEST(test_a_wadi_node_that_restarts_into_a_flood_after_the_all_clear_is_believed_at_once);
  RUN_TEST(test_an_all_clear_still_held_for_a_wadi_node_is_dropped_when_that_node_raises_its_alert_level);
  RUN_TEST(test_a_wadi_node_that_missed_the_all_clear_is_given_it_again_and_no_new_event_starts);
  RUN_TEST(test_a_wadi_node_that_never_hears_is_answered_a_limited_number_of_times_then_starts_a_new_event);
  RUN_TEST(test_an_alert_refresh_above_the_alert_level_the_all_clear_ended_starts_a_new_event);
  RUN_TEST(test_a_gateway_restarted_during_the_event_answers_the_alert_refresh_it_hears_while_holding_the_all_clear);
  RUN_TEST(test_the_gateway_s_airtime_stays_under_the_duty_cycle_limit_even_when_no_wadi_node_hears);
  RUN_TEST(test_a_wadi_node_at_watch_in_dry_mode_hears_the_all_clear_after_its_heartbeat);
  RUN_TEST(test_a_wadi_node_with_no_alert_never_switches_its_receiver_on);
  RUN_TEST(test_a_wadi_node_in_alert_listens_only_for_the_answer_window_after_each_transmission);
  RUN_TEST(test_a_closing_sms_goes_once_to_every_recipient_who_received_an_alert_for_the_event);
  RUN_TEST(test_a_recipient_whose_alert_sms_never_went_out_gets_no_closing_sms);
  RUN_TEST(test_with_gsm_down_the_all_clear_still_goes_out_and_the_closing_sms_follow_once_gsm_is_back);
  RUN_TEST(test_the_all_clear_is_reported_to_the_backend_once_for_each_wadi_node);
  RUN_TEST(test_with_the_uplink_down_after_the_command_the_all_clear_goes_out_and_is_reported_later);
  RUN_TEST(test_a_command_that_waited_too_long_at_the_broker_is_not_acted_on);
  RUN_TEST(test_a_command_that_waited_a_short_while_at_the_broker_is_acted_on);
  RUN_TEST(test_a_command_is_not_acted_on_while_the_gateway_does_not_know_the_time);
  RUN_TEST(test_a_command_dated_in_the_future_is_not_acted_on);
  RUN_TEST(test_a_command_delivered_twice_is_acted_on_once);
  RUN_TEST(test_a_later_command_with_the_number_of_an_earlier_one_is_acted_on);
  RUN_TEST(test_a_command_the_gateway_cannot_make_sense_of_changes_nothing);
  RUN_TEST(test_the_command_of_the_contract_is_read);
  RUN_TEST(test_anything_else_is_refused_whole);
  return UNITY_END();
}
