// Scenario tests for the SMS tier: a wadi node, a siren node and the gateway together,
// asserting the SMS the gateway hands to its GSM modem.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <unity.h>

#include "wadiwatch/alert_engine.h"
#include "wadiwatch/packet.h"
#include "wadiwatch/recipients.h"
#include "wadiwatch/site_config.h"
#include "wadiwatch/sms_templates.h"
#include "wadiwatch_testing/test_site.h"

using wadiwatch::AlertLevel;
using wadiwatch::Language;
using wadiwatch::levelThresholdCm;
using wadiwatch::NodeId;
using wadiwatch::Packet;
using wadiwatch::PacketCodec;
using wadiwatch::PacketKind;
using wadiwatch::Recipient;
using wadiwatch::RecipientGroup;
using wadiwatch::RecipientList;
using wadiwatch::SiteAuth;
using wadiwatch::SiteConfig;
using wadiwatch::WadiConfig;
using wadiwatch::WadiId;
using wadiwatch::testing::FakeStorage;
using wadiwatch::testing::Frame;
using wadiwatch::testing::SirenSound;
using wadiwatch::testing::Sms;
using wadiwatch::testing::TestSite;

void setUp() {}
void tearDown() {}

#define ASSERT_SIREN_SOUND(expected, actual) \
  TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual))

namespace {

// Every phone number in the repository is fake and starts like this: Tunisian
// subscriber numbers never begin with 0, so none of them can reach a real phone.
const char kFakeNumberPrefix[] = "+21600";

// The wadi the scenarios flood, and a second wadi that stays dry.
const WadiId kFloodedWadi = 1;
const WadiId kOtherWadi = 2;

// Officials are told about every wadi, wherever they are registered.
const Recipient kOfficialFrench = {"+21600000101", Language::French, kOtherWadi,
                                   RecipientGroup::Official};
const Recipient kOfficialArabic = {"+21600000102", Language::Arabic, kFloodedWadi,
                                   RecipientGroup::Official};
const Recipient kResidentArabic = {"+21600000201", Language::Arabic, kFloodedWadi,
                                   RecipientGroup::Resident};
const Recipient kResidentFrench = {"+21600000202", Language::French, kFloodedWadi,
                                   RecipientGroup::Resident};
const Recipient kFarResidentArabic = {"+21600000301", Language::Arabic, kOtherWadi,
                                      RecipientGroup::Resident};
const Recipient kFarResidentFrench = {"+21600000302", Language::French, kOtherWadi,
                                      RecipientGroup::Resident};

const Recipient kEveryone[] = {kOfficialFrench,  kOfficialArabic,    kResidentArabic,
                               kResidentFrench,  kFarResidentArabic, kFarResidentFrench};

RecipientList registeredRecipients() {
  RecipientList list = {};
  size_t slot = 0;
  for (const Recipient& recipient : kEveryone) list.recipients[slot++] = recipient;
  return list;
}

// The prototype site with two wadis known to the gateway: the wadi node is
// mounted over the first, the second has no wadi node.
SiteConfig twoWadiSite() {
  SiteConfig config = wadiwatch::prototypeSite();
  const WadiConfig flooded = {kFloodedWadi, "وادي الاختبار", "Oued Essai", {config.wadiNode.id}, 1};
  const WadiConfig other = {kOtherWadi, "وادي الجوار", "Oued Voisin", {}, 0};
  config.gateway.wadis[0] = flooded;
  config.gateway.wadis[1] = other;
  config.gateway.wadiCount = 2;
  return config;
}

struct SmsSite : TestSite {
  SmsSite() : TestSite(twoWadiSite(), registeredRecipients()) {}
  explicit SmsSite(const SiteConfig& config) : TestSite(config, registeredRecipients()) {}
};

const uint64_t kOneHourMs = 60ull * 60 * 1000;
// Design target: an SMS follows its alert within a minute while GSM is available.
const uint64_t kSmsTargetMs = 60ull * 1000;

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

// How many alert packets at `alertLevel` the site's devices have put on the air.
size_t alertsOnAirAt(const TestSite& site, AlertLevel alertLevel) {
  size_t count = 0;
  for (const Packet& packet : site.packetsOnAir(PacketKind::Alert)) {
    if (packet.alert.alertLevel == alertLevel) ++count;
  }
  return count;
}

// The wadi node is power-cycled with the wadi as it is. It measures the level and raises
// its alert again: a new packet, which every receiver accepts, not a copy of an earlier one.
void raiseAlertAgain(TestSite& site) {
  const size_t alertsBefore = site.packetsOnAir(PacketKind::Alert).size();
  site.restartWadiNode();
  for (uint64_t waited = 0; site.packetsOnAir(PacketKind::Alert).size() == alertsBefore;
       waited += TestSite::kStepMs) {
    TEST_ASSERT_TRUE_MESSAGE(waited < kOneHourMs, "the wadi node did not raise its alert again");
    site.runMs(TestSite::kStepMs);
  }
}

// What a third radio would transmit: an Evacuate alert claiming to come from `claimedSender`,
// signed with whatever key the transmitter holds. It is that transmitter's first packet.
Frame signedAlert(const SiteAuth& auth, NodeId claimedSender) {
  FakeStorage storage;
  PacketCodec codec(auth, claimedSender, storage);
  Packet packet = Packet();
  packet.kind = PacketKind::Alert;
  packet.alert.alertLevel = AlertLevel::Evacuate;
  uint8_t buffer[wadiwatch::kMaxPacketSize];
  const size_t length = codec.encode(packet, buffer, sizeof(buffer));
  return Frame(buffer, buffer + length);
}

// The longest that the further transmissions of an alert can take, counted from the first.
uint64_t repeatWindowMs(const TestSite& site) {
  const wadiwatch::AlertRepeatConfig& repeat = site.config.wadiNode.alertRepeat;
  return static_cast<uint64_t>(repeat.transmissions - 1) * repeat.maxSpacingMs;
}

// The siren node's heartbeats. The wadi node sends heartbeats of its own.
std::vector<Packet> sirenNodeHeartbeats(const TestSite& site) {
  std::vector<Packet> heartbeats;
  for (const Packet& packet : site.packetsOnAirFrom(site.config.sirenNode.id)) {
    if (packet.kind == PacketKind::Heartbeat) heartbeats.push_back(packet);
  }
  return heartbeats;
}

// Gives the gateway the time it is allowed to hand its SMS to the modem.
void letSmsGoOut(TestSite& site) { site.runMs(kSmsTargetMs); }

// Long enough after the end of a GSM outage for the gateway to have retried.
void letSmsGoOutAfterOutage(TestSite& site) {
  site.runMs(site.config.gateway.smsRetryIntervalMs + kSmsTargetMs);
}

std::vector<std::string> textsSentTo(const TestSite& site, const Recipient& recipient) {
  std::vector<std::string> texts;
  for (const Sms& sms : site.modem.sent()) {
    if (sms.phoneNumber == recipient.phoneNumber) texts.push_back(sms.text);
  }
  return texts;
}

size_t smsCountTo(const TestSite& site, const Recipient& recipient) {
  return textsSentTo(site, recipient).size();
}

const char* wadiNameIn(const WadiConfig& wadi, Language language) {
  return language == Language::Arabic ? wadi.nameArabic : wadi.nameFrench;
}

// The SMS a recipient should read when `wadi` reaches `alertLevel`.
std::string expectedText(const Recipient& recipient, const WadiConfig& wadi, AlertLevel alertLevel) {
  char text[wadiwatch::kMaxSmsTextLength + 1];
  const size_t length = wadiwatch::renderAlertSms(recipient.language, alertLevel,
                                                  wadiNameIn(wadi, recipient.language), text,
                                                  sizeof(text));
  return std::string(text, length);
}

bool contains(const std::string& text, const char* part) {
  return text.find(part) != std::string::npos;
}

// True when the text has at least one letter of the Arabic script (U+0600 to U+06FF).
bool hasArabicScript(const std::string& text) {
  for (const char c : text) {
    const uint8_t byte = static_cast<uint8_t>(c);
    if (byte >= 0xD8 && byte <= 0xDB) return true;
  }
  return false;
}

}  // namespace

static void test_a_wadi_below_the_watch_threshold_sends_no_sms() {
  SmsSite site;
  site.levelSensor.setWaterLevelCm(thresholdCm(site, AlertLevel::Watch) / 2.0f);

  site.runMinutes(30);

  TEST_ASSERT_EQUAL_UINT(0, site.modem.sent().size());
}

static void test_watch_sends_sms_to_officials_only() {
  SmsSite site;

  floodTo(site, AlertLevel::Watch);
  letSmsGoOut(site);

  TEST_ASSERT_EQUAL_UINT(1, smsCountTo(site, kOfficialFrench));
  TEST_ASSERT_EQUAL_UINT(1, smsCountTo(site, kOfficialArabic));
  TEST_ASSERT_EQUAL_UINT(2, site.modem.sent().size());
}

static void test_warning_sends_sms_to_officials_and_to_residents_near_that_wadi() {
  SmsSite site;

  floodTo(site, AlertLevel::Warning);
  letSmsGoOut(site);

  TEST_ASSERT_EQUAL_UINT(1, smsCountTo(site, kOfficialFrench));
  TEST_ASSERT_EQUAL_UINT(1, smsCountTo(site, kOfficialArabic));
  TEST_ASSERT_EQUAL_UINT(1, smsCountTo(site, kResidentArabic));
  TEST_ASSERT_EQUAL_UINT(1, smsCountTo(site, kResidentFrench));
  // Residents of the other wadi are not disturbed.
  TEST_ASSERT_EQUAL_UINT(0, smsCountTo(site, kFarResidentArabic));
  TEST_ASSERT_EQUAL_UINT(0, smsCountTo(site, kFarResidentFrench));
  TEST_ASSERT_EQUAL_UINT(4, site.modem.sent().size());
}

static void test_warning_follows_the_wadi_the_wadi_node_is_mounted_over() {
  // The same wadi node, mounted over the other wadi instead.
  SiteConfig config = twoWadiSite();
  config.gateway.wadis[0].wadiNodeCount = 0;
  config.gateway.wadis[1].wadiNodes[0] = config.wadiNode.id;
  config.gateway.wadis[1].wadiNodeCount = 1;
  SmsSite site(config);

  floodTo(site, AlertLevel::Warning);
  letSmsGoOut(site);

  TEST_ASSERT_EQUAL_UINT(1, smsCountTo(site, kOfficialFrench));
  TEST_ASSERT_EQUAL_UINT(1, smsCountTo(site, kOfficialArabic));
  TEST_ASSERT_EQUAL_UINT(1, smsCountTo(site, kFarResidentArabic));
  TEST_ASSERT_EQUAL_UINT(1, smsCountTo(site, kFarResidentFrench));
  TEST_ASSERT_EQUAL_UINT(0, smsCountTo(site, kResidentArabic));
  TEST_ASSERT_EQUAL_UINT(0, smsCountTo(site, kResidentFrench));
  // And the SMS names that wadi.
  const std::string text = textsSentTo(site, kFarResidentFrench).front();
  TEST_ASSERT_TRUE(contains(text, config.gateway.wadis[1].nameFrench));
  TEST_ASSERT_FALSE(contains(text, config.gateway.wadis[0].nameFrench));
}

static void test_evacuate_sends_sms_to_all_registered_recipients() {
  SmsSite site;

  floodTo(site, AlertLevel::Evacuate);
  letSmsGoOut(site);

  for (const Recipient& recipient : kEveryone) {
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, smsCountTo(site, recipient), recipient.phoneNumber);
  }
  TEST_ASSERT_EQUAL_UINT(6, site.modem.sent().size());
}

static void test_each_change_of_alert_level_sends_one_sms_to_each_recipient_it_concerns() {
  SmsSite site;

  floodTo(site, AlertLevel::Watch);
  letSmsGoOut(site);
  floodTo(site, AlertLevel::Warning);
  letSmsGoOut(site);
  floodTo(site, AlertLevel::Evacuate);
  letSmsGoOut(site);

  // Officials hear of all three levels, residents near the wadi of Warning and
  // Evacuate, residents of the other wadi of Evacuate alone.
  const WadiConfig& wadi = site.config.gateway.wadis[0];
  const std::vector<std::string> official = textsSentTo(site, kOfficialArabic);
  TEST_ASSERT_EQUAL_UINT(3, official.size());
  TEST_ASSERT_EQUAL_STRING(expectedText(kOfficialArabic, wadi, AlertLevel::Watch).c_str(),
                           official[0].c_str());
  TEST_ASSERT_EQUAL_STRING(expectedText(kOfficialArabic, wadi, AlertLevel::Warning).c_str(),
                           official[1].c_str());
  TEST_ASSERT_EQUAL_STRING(expectedText(kOfficialArabic, wadi, AlertLevel::Evacuate).c_str(),
                           official[2].c_str());

  const std::vector<std::string> resident = textsSentTo(site, kResidentFrench);
  TEST_ASSERT_EQUAL_UINT(2, resident.size());
  TEST_ASSERT_EQUAL_STRING(expectedText(kResidentFrench, wadi, AlertLevel::Warning).c_str(),
                           resident[0].c_str());
  TEST_ASSERT_EQUAL_STRING(expectedText(kResidentFrench, wadi, AlertLevel::Evacuate).c_str(),
                           resident[1].c_str());

  const std::vector<std::string> farResident = textsSentTo(site, kFarResidentArabic);
  TEST_ASSERT_EQUAL_UINT(1, farResident.size());
  TEST_ASSERT_EQUAL_STRING(expectedText(kFarResidentArabic, wadi, AlertLevel::Evacuate).c_str(),
                           farResident[0].c_str());

  TEST_ASSERT_EQUAL_UINT(3 + 3 + 2 + 2 + 1 + 1, site.modem.sent().size());
}

static void test_repeated_alert_packets_at_the_same_level_send_no_additional_sms() {
  SmsSite site;
  floodTo(site, AlertLevel::Watch);
  letSmsGoOut(site);
  floodTo(site, AlertLevel::Warning);
  letSmsGoOut(site);
  const size_t sentForTheTwoLevels = site.modem.sent().size();
  TEST_ASSERT_EQUAL_UINT(2 + 4, sentForTheTwoLevels);
  TEST_ASSERT_EQUAL_UINT(1, alertsOnAirAt(site, AlertLevel::Warning));

  // The alert stays in force for half an hour. Every few minutes the wadi node raises
  // Warning again in a new packet, and every frame of the event so far (the earlier
  // Watch among them) is transmitted again byte for byte.
  const size_t repeats = 5;
  for (size_t repeat = 0; repeat < repeats; ++repeat) {
    raiseAlertAgain(site);
    const std::vector<Frame> heardSoFar = site.air.transmitted();
    for (const Frame& frame : heardSoFar) site.air.inject(frame);
    site.runMinutes(6);
  }

  // The Warning went out again each time, in packets a receiver accepts, and nobody
  // was sent a second SMS for it.
  TEST_ASSERT_EQUAL_UINT(1 + repeats, alertsOnAirAt(site, AlertLevel::Warning));
  TEST_ASSERT_EQUAL_UINT(sentForTheTwoLevels, site.modem.sent().size());

  // Nor does a new Watch packet, heard while the Warning is in force, send anything.
  site.levelSensor.setWaterLevelCm(levelWithin(site, AlertLevel::Watch));
  raiseAlertAgain(site);
  TEST_ASSERT_EQUAL_UINT(2, alertsOnAirAt(site, AlertLevel::Watch));
  letSmsGoOut(site);
  TEST_ASSERT_EQUAL_UINT(sentForTheTwoLevels, site.modem.sent().size());
}

static void test_each_recipient_receives_the_text_in_their_registered_language() {
  SmsSite site;
  const WadiConfig& wadi = site.config.gateway.wadis[0];

  floodTo(site, AlertLevel::Evacuate);
  letSmsGoOut(site);

  for (const Recipient& recipient : kEveryone) {
    const std::vector<std::string> texts = textsSentTo(site, recipient);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, texts.size(), recipient.phoneNumber);
    const bool arabicExpected = recipient.language == Language::Arabic;
    TEST_ASSERT_EQUAL_MESSAGE(arabicExpected, hasArabicScript(texts[0]), recipient.phoneNumber);
    TEST_ASSERT_EQUAL_STRING(expectedText(recipient, wadi, AlertLevel::Evacuate).c_str(),
                             texts[0].c_str());
  }
}

static void test_sms_names_the_wadi_and_the_alert_level_in_the_recipient_s_language() {
  SmsSite site;
  const WadiConfig& wadi = site.config.gateway.wadis[0];

  floodTo(site, AlertLevel::Warning);
  letSmsGoOut(site);

  const std::string french = textsSentTo(site, kResidentFrench).front();
  TEST_ASSERT_TRUE(contains(french, wadi.nameFrench));
  TEST_ASSERT_TRUE(
      contains(french, wadiwatch::alertLevelName(Language::French, AlertLevel::Warning)));
  TEST_ASSERT_FALSE(contains(french, wadi.nameArabic));

  const std::string arabic = textsSentTo(site, kResidentArabic).front();
  TEST_ASSERT_TRUE(contains(arabic, wadi.nameArabic));
  TEST_ASSERT_TRUE(
      contains(arabic, wadiwatch::alertLevelName(Language::Arabic, AlertLevel::Warning)));
  TEST_ASSERT_FALSE(contains(arabic, wadi.nameFrench));
}

static void test_sms_queued_during_a_gsm_outage_are_each_sent_exactly_once_on_recovery() {
  SmsSite site;
  site.modem.startOutage();

  floodTo(site, AlertLevel::Warning);
  site.runMinutes(10);

  // Nothing got through, though the gateway asked; the siren does not depend on GSM.
  TEST_ASSERT_EQUAL_UINT(0, site.modem.sent().size());
  TEST_ASSERT_TRUE(site.modem.refused().size() > 0);
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());

  site.modem.endOutage();
  letSmsGoOutAfterOutage(site);

  TEST_ASSERT_EQUAL_UINT(1, smsCountTo(site, kOfficialFrench));
  TEST_ASSERT_EQUAL_UINT(1, smsCountTo(site, kOfficialArabic));
  TEST_ASSERT_EQUAL_UINT(1, smsCountTo(site, kResidentArabic));
  TEST_ASSERT_EQUAL_UINT(1, smsCountTo(site, kResidentFrench));
  TEST_ASSERT_EQUAL_UINT(4, site.modem.sent().size());

  // And none of them is sent again later.
  site.runMinutes(30);
  TEST_ASSERT_EQUAL_UINT(4, site.modem.sent().size());
}

static void test_a_gsm_outage_that_starts_part_way_through_resends_nothing_already_sent() {
  SmsSite site;
  floodTo(site, AlertLevel::Evacuate);
  for (uint64_t waited = 0; site.modem.sent().empty(); waited += TestSite::kStepMs) {
    TEST_ASSERT_TRUE_MESSAGE(waited < kSmsTargetMs, "no SMS was sent");
    site.runMs(TestSite::kStepMs);
  }

  site.modem.startOutage();
  site.runMinutes(5);
  site.modem.endOutage();
  letSmsGoOutAfterOutage(site);

  for (const Recipient& recipient : kEveryone) {
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, smsCountTo(site, recipient), recipient.phoneNumber);
  }
  TEST_ASSERT_EQUAL_UINT(6, site.modem.sent().size());
}

static void test_an_sms_overtaken_by_a_higher_alert_level_during_an_outage_is_replaced_by_it() {
  SmsSite site;
  const WadiConfig& wadi = site.config.gateway.wadis[0];
  site.modem.startOutage();

  floodTo(site, AlertLevel::Watch);
  site.runMinutes(2);
  floodTo(site, AlertLevel::Warning);
  site.runMinutes(2);
  floodTo(site, AlertLevel::Evacuate);
  site.runMinutes(2);
  site.modem.endOutage();
  letSmsGoOutAfterOutage(site);

  // On recovery each recipient is told what is true now, once, not the backlog.
  for (const Recipient& recipient : kEveryone) {
    const std::vector<std::string> texts = textsSentTo(site, recipient);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, texts.size(), recipient.phoneNumber);
    TEST_ASSERT_EQUAL_STRING(expectedText(recipient, wadi, AlertLevel::Evacuate).c_str(),
                             texts[0].c_str());
  }
}

static void test_during_a_gsm_outage_the_gateway_retries_at_the_configured_interval() {
  SmsSite site;
  const uint64_t retryIntervalMs = site.config.gateway.smsRetryIntervalMs;
  site.modem.startOutage();
  floodTo(site, AlertLevel::Warning);
  site.runMs(retryIntervalMs);

  const size_t refusedBefore = site.modem.refused().size();
  const uint64_t intervals = 10;
  site.runMs(intervals * retryIntervalMs);

  // It keeps trying, but one SMS per interval rather than hammering the modem.
  const size_t attempts = site.modem.refused().size() - refusedBefore;
  TEST_ASSERT_TRUE(attempts >= intervals - 1);
  TEST_ASSERT_TRUE(attempts <= intervals + 1);
}

static void test_a_number_the_network_refuses_does_not_hold_up_the_other_recipients() {
  SmsSite site;
  site.modem.rejectNumber(kOfficialFrench.phoneNumber);

  floodTo(site, AlertLevel::Evacuate);
  letSmsGoOutAfterOutage(site);

  // The first recipient of the list never gets through; everyone after them does.
  TEST_ASSERT_EQUAL_UINT(0, smsCountTo(site, kOfficialFrench));
  TEST_ASSERT_TRUE(site.modem.refused().size() > 0);
  TEST_ASSERT_EQUAL_UINT(1, smsCountTo(site, kOfficialArabic));
  TEST_ASSERT_EQUAL_UINT(1, smsCountTo(site, kResidentArabic));
  TEST_ASSERT_EQUAL_UINT(1, smsCountTo(site, kResidentFrench));
  TEST_ASSERT_EQUAL_UINT(1, smsCountTo(site, kFarResidentArabic));
  TEST_ASSERT_EQUAL_UINT(1, smsCountTo(site, kFarResidentFrench));
}

static void test_with_the_gateway_absent_the_siren_still_sounds() {
  SmsSite site;
  site.unplugGateway();

  floodTo(site, AlertLevel::Warning);
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());
  TEST_ASSERT_TRUE(site.siren.isStrobeOn());

  floodTo(site, AlertLevel::Evacuate);
  ASSERT_SIREN_SOUND(SirenSound::Continuous, site.listenToSiren());
  TEST_ASSERT_TRUE(site.siren.isStrobeOn());

  // The information path is down with the gateway: no SMS.
  letSmsGoOut(site);
  TEST_ASSERT_EQUAL_UINT(0, site.modem.sent().size());
  TEST_ASSERT_EQUAL_UINT(0, site.modem.refused().size());
}

static void test_an_alert_from_a_wadi_node_mounted_over_no_wadi_the_gateway_knows_sends_no_sms() {
  // The wadi node belongs to the site, so its alert is believed, but the gateway
  // knows of no wadi it is mounted over: there is nobody to tell.
  SiteConfig config = twoWadiSite();
  config.gateway.wadis[0].wadiNodeCount = 0;
  SmsSite site(config);

  floodTo(site, AlertLevel::Evacuate);
  letSmsGoOut(site);

  ASSERT_SIREN_SOUND(SirenSound::Continuous, site.listenToSiren());
  TEST_ASSERT_EQUAL_UINT(0, site.modem.sent().size());
  TEST_ASSERT_EQUAL_UINT(0, site.modem.refused().size());
}

// Control for the three scenarios below: the same alert, signed with the site key in
// the name of the site's wadi node, does send SMS.
static void test_an_alert_signed_with_the_site_key_by_the_wadi_node_sends_sms() {
  SmsSite site;

  site.air.inject(signedAlert(site.config.auth, site.config.wadiNode.id));
  letSmsGoOut(site);

  TEST_ASSERT_EQUAL_UINT(6, site.modem.sent().size());
}

static void test_a_forged_alert_sends_no_sms() {
  SmsSite site;
  SiteAuth forgerAuth = site.config.auth;
  forgerAuth.key[0] ^= 0x01;

  site.air.inject(signedAlert(forgerAuth, site.config.wadiNode.id));
  letSmsGoOut(site);

  TEST_ASSERT_EQUAL_UINT(0, site.modem.sent().size());
  TEST_ASSERT_EQUAL_UINT(0, site.modem.refused().size());
}

static void test_a_signed_alert_from_a_node_outside_the_site_sends_no_sms() {
  // The transmitter holds the site key but is not in the site's node list. Even a
  // gateway told that this identifier is a wadi node over one of its wadis does not
  // act: the node list decides who is believed, for the gateway as for the siren node.
  SiteConfig config = twoWadiSite();
  const NodeId outsider = SmsSite(config).nodeIdOutsideSite();
  config.gateway.wadis[1].wadiNodes[0] = outsider;
  config.gateway.wadis[1].wadiNodeCount = 1;
  SmsSite site(config);

  site.air.inject(signedAlert(site.config.auth, outsider));
  letSmsGoOut(site);

  TEST_ASSERT_EQUAL_UINT(0, site.modem.sent().size());
  TEST_ASSERT_EQUAL_UINT(0, site.modem.refused().size());
}

static void test_a_replayed_alert_sends_no_sms() {
  SmsSite site;
  const Frame alert = signedAlert(site.config.auth, site.config.wadiNode.id);
  site.air.inject(alert);
  letSmsGoOut(site);
  TEST_ASSERT_EQUAL_UINT(6, site.modem.sent().size());

  // The gateway is power-cycled. It no longer knows of the event, so Evacuate would be
  // news to it; but it still knows which packets it has accepted.
  site.restartGateway();
  site.air.inject(alert);
  letSmsGoOut(site);

  TEST_ASSERT_EQUAL_UINT(6, site.modem.sent().size());
  TEST_ASSERT_EQUAL_UINT(0, site.modem.refused().size());
}

static void test_malformed_frames_send_no_sms() {
  SmsSite site;

  site.air.inject({0xFF, 0x00, 0x13, 0x37});
  site.air.inject({});
  site.air.inject(Frame(200, 0x01));
  site.runMinutes(5);

  TEST_ASSERT_EQUAL_UINT(0, site.modem.sent().size());

  // And the gateway still does its job afterwards.
  floodTo(site, AlertLevel::Watch);
  letSmsGoOut(site);
  TEST_ASSERT_EQUAL_UINT(2, site.modem.sent().size());
}

// In storm mode the wadi node sends a reading with every measurement. The gateway hears
// them all; they are measurements for the dashboard, not alerts.
static void test_readings_from_a_storm_that_raises_no_alert_send_no_sms() {
  SmsSite site;
  site.levelSensor.setWaterLevelCm(thresholdCm(site, AlertLevel::Watch) / 2.0f);

  // A little rain, far short of the Watch rainfall: storm mode and nothing more.
  site.rainGauge.tip();
  site.runMinutes(20);

  TEST_ASSERT_TRUE(site.packetsOnAir(PacketKind::Reading).size() > 0);
  TEST_ASSERT_EQUAL_UINT(0, site.packetsOnAir(PacketKind::Alert).size());
  TEST_ASSERT_EQUAL_UINT(0, site.modem.sent().size());
  TEST_ASSERT_EQUAL_UINT(0, site.modem.refused().size());

  // And the alert that follows all those readings is acted on as usual.
  floodTo(site, AlertLevel::Watch);
  letSmsGoOut(site);
  TEST_ASSERT_EQUAL_UINT(1, smsCountTo(site, kOfficialFrench));
  TEST_ASSERT_EQUAL_UINT(1, smsCountTo(site, kOfficialArabic));
  TEST_ASSERT_EQUAL_UINT(2, site.modem.sent().size());
}

static void test_a_reading_that_carries_an_alert_level_is_not_an_alert() {
  SmsSite site;
  floodTo(site, AlertLevel::Warning);
  letSmsGoOut(site);
  TEST_ASSERT_EQUAL_UINT(4, site.modem.sent().size());

  // The gateway is power-cycled and no longer knows of the event. The readings it hears
  // from now on say Warning, which would be news to it; but only an alert packet is.
  site.restartGateway();
  const size_t readingsBefore = site.packetsOnAir(PacketKind::Reading).size();
  site.runMinutes(10);

  const std::vector<Packet> readings = site.packetsOnAir(PacketKind::Reading);
  TEST_ASSERT_TRUE(readings.size() > readingsBefore);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertLevel::Warning),
                        static_cast<int>(readings.back().reading.alertLevel));
  TEST_ASSERT_EQUAL_UINT(1, alertsOnAirAt(site, AlertLevel::Warning));
  TEST_ASSERT_EQUAL_UINT(4, site.modem.sent().size());
  TEST_ASSERT_EQUAL_UINT(0, site.modem.refused().size());

  // The next alert packet starts the event the gateway had forgotten.
  floodTo(site, AlertLevel::Evacuate);
  letSmsGoOut(site);
  TEST_ASSERT_EQUAL_UINT(4 + 6, site.modem.sent().size());
}

// The wadi node transmits every alert several times and the siren node answers each
// alert with an acknowledgement. The gateway hears all of it; a change of alert
// level is still one SMS to each recipient it concerns.
static void test_an_alert_transmitted_several_times_and_acknowledged_sends_each_recipient_one_sms() {
  SmsSite site;
  const size_t transmissions = site.config.wadiNode.alertRepeat.transmissions;
  TEST_ASSERT_TRUE(transmissions > 1);

  const AlertLevel alertLevels[] = {AlertLevel::Watch, AlertLevel::Warning, AlertLevel::Evacuate};
  for (const AlertLevel alertLevel : alertLevels) {
    floodTo(site, alertLevel);
    site.runMs(repeatWindowMs(site));
    letSmsGoOut(site);
  }

  // Three alerts went out, each as many times as configured, and each was acknowledged.
  const std::vector<Frame> alerts = site.framesOnAir(PacketKind::Alert);
  TEST_ASSERT_EQUAL_UINT(3, alerts.size());
  for (const Frame& alert : alerts) {
    TEST_ASSERT_EQUAL_UINT(transmissions, site.timesTransmitted(alert));
  }
  const std::vector<Packet> acknowledgements = site.packetsOnAir(PacketKind::Acknowledgement);
  TEST_ASSERT_EQUAL_UINT(3, acknowledgements.size());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertLevel::Evacuate),
                        static_cast<int>(acknowledgements.back().acknowledgement.alertLevel));

  // Officials heard of all three levels, residents near the wadi of two, the rest of one.
  TEST_ASSERT_EQUAL_UINT(3, smsCountTo(site, kOfficialFrench));
  TEST_ASSERT_EQUAL_UINT(3, smsCountTo(site, kOfficialArabic));
  TEST_ASSERT_EQUAL_UINT(2, smsCountTo(site, kResidentArabic));
  TEST_ASSERT_EQUAL_UINT(2, smsCountTo(site, kResidentFrench));
  TEST_ASSERT_EQUAL_UINT(1, smsCountTo(site, kFarResidentArabic));
  TEST_ASSERT_EQUAL_UINT(1, smsCountTo(site, kFarResidentFrench));
  TEST_ASSERT_EQUAL_UINT(3 + 3 + 2 + 2 + 1 + 1, site.modem.sent().size());
  TEST_ASSERT_EQUAL_UINT(0, site.modem.refused().size());
}

static void test_with_the_first_transmission_of_an_alert_lost_its_sms_go_out_once_on_a_repeat() {
  SmsSite site;
  site.air.dropNext(1);

  floodTo(site, AlertLevel::Evacuate);

  // The alert has gone out once and was lost: the gateway has nothing to send.
  const Frame alert = site.framesOnAir(PacketKind::Alert).front();
  TEST_ASSERT_EQUAL_UINT(1, site.timesTransmitted(alert));
  TEST_ASSERT_EQUAL_UINT(0, site.modem.sent().size());

  site.runMs(repeatWindowMs(site));
  letSmsGoOut(site);

  // It heard one of the further transmissions, and the one after that changed nothing.
  TEST_ASSERT_EQUAL_UINT(site.config.wadiNode.alertRepeat.transmissions,
                         site.timesTransmitted(alert));
  for (const Recipient& recipient : kEveryone) {
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, smsCountTo(site, recipient), recipient.phoneNumber);
  }
  TEST_ASSERT_EQUAL_UINT(6, site.modem.sent().size());
}

// A siren node transmits too: a test packet when its button is pressed, an hourly
// heartbeat, an acknowledgement that names an alert level, and a heartbeat that
// reports a lost alert refresh. None of them is an alert.
static void test_what_the_siren_node_transmits_sends_no_sms() {
  SmsSite site;
  const wadiwatch::SirenNodeConfig& sirenNode = site.config.sirenNode;

  // With no alert anywhere: a test from the button, and a heartbeat.
  site.sirenButton.press();
  site.runMs(sirenNode.heartbeatIntervalMs + kSmsTargetMs);
  TEST_ASSERT_EQUAL_UINT(1, site.packetsOnAir(PacketKind::Test).size());
  TEST_ASSERT_FALSE(sirenNodeHeartbeats(site).empty());
  TEST_ASSERT_EQUAL_UINT(0, site.modem.sent().size());
  TEST_ASSERT_EQUAL_UINT(0, site.modem.refused().size());

  // During an event the Warning is told once.
  floodTo(site, AlertLevel::Warning);
  letSmsGoOut(site);
  TEST_ASSERT_EQUAL_UINT(4, site.modem.sent().size());

  // The wadi node is lost. The siren node waits for a refresh, gives up, says so in a
  // heartbeat and goes on saying so every hour.
  site.unplugWadiNode();
  site.runMs(static_cast<uint64_t>(sirenNode.alertRefreshTimeoutMs) + sirenNode.heartbeatIntervalMs);

  TEST_ASSERT_EQUAL_UINT(1, site.packetsOnAir(PacketKind::Acknowledgement).size());
  TEST_ASSERT_TRUE(sirenNodeHeartbeats(site).back().heartbeat.alertRefreshLost);
  TEST_ASSERT_EQUAL_UINT(4, site.modem.sent().size());
  TEST_ASSERT_EQUAL_UINT(0, site.modem.refused().size());
}

static void test_on_the_prototype_site_evacuate_reaches_every_committed_recipient() {
  TestSite site;
  const RecipientList& committed = wadiwatch::prototypeRecipients();

  floodTo(site, AlertLevel::Evacuate);
  letSmsGoOut(site);

  size_t registered = 0;
  for (const Recipient& recipient : committed.recipients) {
    if (recipient.phoneNumber[0] == '\0') continue;
    ++registered;
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, smsCountTo(site, recipient), recipient.phoneNumber);
  }
  TEST_ASSERT_TRUE(registered > 0);
  TEST_ASSERT_EQUAL_UINT(registered, site.modem.sent().size());
}

static void test_the_committed_recipient_list_holds_only_fake_numbers() {
  const RecipientList& committed = wadiwatch::prototypeRecipients();

  for (const Recipient& recipient : committed.recipients) {
    if (recipient.phoneNumber[0] == '\0') continue;
    TEST_ASSERT_EQUAL_STRING_LEN_MESSAGE(
        kFakeNumberPrefix, recipient.phoneNumber, std::strlen(kFakeNumberPrefix),
        "a phone number that is not marked as fake is committed to the repository");
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_wadi_below_the_watch_threshold_sends_no_sms);
  RUN_TEST(test_watch_sends_sms_to_officials_only);
  RUN_TEST(test_warning_sends_sms_to_officials_and_to_residents_near_that_wadi);
  RUN_TEST(test_warning_follows_the_wadi_the_wadi_node_is_mounted_over);
  RUN_TEST(test_evacuate_sends_sms_to_all_registered_recipients);
  RUN_TEST(test_each_change_of_alert_level_sends_one_sms_to_each_recipient_it_concerns);
  RUN_TEST(test_repeated_alert_packets_at_the_same_level_send_no_additional_sms);
  RUN_TEST(test_each_recipient_receives_the_text_in_their_registered_language);
  RUN_TEST(test_sms_names_the_wadi_and_the_alert_level_in_the_recipient_s_language);
  RUN_TEST(test_sms_queued_during_a_gsm_outage_are_each_sent_exactly_once_on_recovery);
  RUN_TEST(test_a_gsm_outage_that_starts_part_way_through_resends_nothing_already_sent);
  RUN_TEST(test_an_sms_overtaken_by_a_higher_alert_level_during_an_outage_is_replaced_by_it);
  RUN_TEST(test_during_a_gsm_outage_the_gateway_retries_at_the_configured_interval);
  RUN_TEST(test_a_number_the_network_refuses_does_not_hold_up_the_other_recipients);
  RUN_TEST(test_with_the_gateway_absent_the_siren_still_sounds);
  RUN_TEST(test_an_alert_from_a_wadi_node_mounted_over_no_wadi_the_gateway_knows_sends_no_sms);
  RUN_TEST(test_an_alert_signed_with_the_site_key_by_the_wadi_node_sends_sms);
  RUN_TEST(test_a_forged_alert_sends_no_sms);
  RUN_TEST(test_a_signed_alert_from_a_node_outside_the_site_sends_no_sms);
  RUN_TEST(test_a_replayed_alert_sends_no_sms);
  RUN_TEST(test_malformed_frames_send_no_sms);
  RUN_TEST(test_readings_from_a_storm_that_raises_no_alert_send_no_sms);
  RUN_TEST(test_a_reading_that_carries_an_alert_level_is_not_an_alert);
  RUN_TEST(test_an_alert_transmitted_several_times_and_acknowledged_sends_each_recipient_one_sms);
  RUN_TEST(test_with_the_first_transmission_of_an_alert_lost_its_sms_go_out_once_on_a_repeat);
  RUN_TEST(test_what_the_siren_node_transmits_sends_no_sms);
  RUN_TEST(test_on_the_prototype_site_evacuate_reaches_every_committed_recipient);
  RUN_TEST(test_the_committed_recipient_list_holds_only_fake_numbers);
  return UNITY_END();
}
