// Scenario tests for reliable delivery on the life-safety path: an alert is repeated
// until it arrives, acknowledged, and kept in force by the siren node until an all
// clear; and the siren node's own button and heartbeat.
//
// Every duration comes from the site configuration. The scenarios watch the site
// step by step, so they assume the configured durations are whole steps.
#include <cstdint>
#include <vector>

#include <unity.h>

#include "wadiwatch/alert_engine.h"
#include "wadiwatch/packet.h"
#include "wadiwatch_testing/test_site.h"

using wadiwatch::AlertLevel;
using wadiwatch::levelThresholdCm;
using wadiwatch::NodeId;
using wadiwatch::Packet;
using wadiwatch::PacketKind;
using wadiwatch::SiteAuth;
using wadiwatch::testing::FakeRandom;
using wadiwatch::testing::Frame;
using wadiwatch::testing::ScriptedNode;
using wadiwatch::testing::SirenSound;
using wadiwatch::testing::TestSite;

void setUp() {}
void tearDown() {}

#define ASSERT_ALERT_LEVEL(expected, actual) \
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected), static_cast<uint8_t>(actual))

#define ASSERT_SIREN_SOUND(expected, actual) \
  TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual))

namespace {

// A level that is clearly at `alertLevel` and clearly short of the next one up.
float levelWithin(const TestSite& site, AlertLevel alertLevel) {
  const wadiwatch::AlertThresholds& thresholds = site.config.wadiNode.alertThresholds;
  const float lowerCm = levelThresholdCm(thresholds, alertLevel);
  const float upperCm =
      alertLevel == AlertLevel::Evacuate
          ? thresholds.bankFullDepthCm
          : levelThresholdCm(thresholds,
                             static_cast<AlertLevel>(static_cast<uint8_t>(alertLevel) + 1));
  return (lowerCm + upperCm) / 2.0f;
}

// The wadi rises to `alertLevel` and stays there until the wadi node has put that
// alert level on the air, whichever operating mode it is in. On return the alert
// has just been transmitted for the first time.
void floodTo(TestSite& site, AlertLevel alertLevel) {
  site.levelSensor.setWaterLevelCm(levelWithin(site, alertLevel));
  for (uint64_t waited = 0; site.latestAlertOnAir() != alertLevel; waited += TestSite::kStepMs) {
    TEST_ASSERT_TRUE_MESSAGE(waited < site.config.wadiNode.mode.dryMeasureIntervalMs,
                             "the wadi node never raised the alert level");
    site.runMs(TestSite::kStepMs);
  }
}

// The longest that the further transmissions of an alert can take, counted from the first.
uint64_t repeatWindowMs(const TestSite& site) {
  const wadiwatch::AlertRepeatConfig& repeat = site.config.wadiNode.alertRepeat;
  return static_cast<uint64_t>(repeat.transmissions - 1) * repeat.maxSpacingMs;
}

uint64_t patternPeriodMs(const TestSite& site) {
  return static_cast<uint64_t>(site.config.sirenNode.intermittentOnMs) +
         site.config.sirenNode.intermittentOffMs;
}

// Whether the siren sounds at any moment of the next `durationMs`.
bool sirenSoundsWithin(TestSite& site, uint64_t durationMs) {
  bool sounded = false;
  for (uint64_t elapsed = 0; elapsed < durationMs; elapsed += TestSite::kStepMs) {
    site.runMs(TestSite::kStepMs);
    sounded = sounded || site.siren.isOn();
  }
  return sounded;
}

// The next transmission of `alert` is lost on the air. Returns in the step in which
// the wadi node made it.
void loseNextTransmissionOf(TestSite& site, const Frame& alert) {
  const size_t transmissionsBefore = site.timesTransmitted(alert);
  const size_t framesBefore = site.air.transmitted().size();
  site.air.dropNext(1);
  for (uint64_t waited = 0; site.timesTransmitted(alert) == transmissionsBefore;
       waited += TestSite::kStepMs) {
    TEST_ASSERT_TRUE_MESSAGE(waited < site.config.wadiNode.alertRepeat.maxSpacingMs,
                             "the alert was not transmitted again");
    site.runMs(TestSite::kStepMs);
  }
  // The frame that was lost is the alert, not something else the site transmitted first.
  TEST_ASSERT_TRUE(site.air.transmitted()[framesBefore] == alert);
}

// On a site whose wadi node draws `draws` from its random source, the wadi floods
// to Warning and the wadi node transmits its alert. Returns the waits between one
// transmission of that alert and the next.
std::vector<uint64_t> alertSpacingsMs(const std::vector<uint32_t>& draws) {
  TestSite site;
  site.wadiNodeRandom.setDraws(draws);
  floodTo(site, AlertLevel::Warning);
  const Frame alert = site.framesOnAir(PacketKind::Alert).front();
  size_t transmissions = site.timesTransmitted(alert);
  std::vector<uint64_t> spacingsMs;
  uint64_t sinceLastMs = 0;
  for (uint64_t elapsed = 0; elapsed < 2 * repeatWindowMs(site); elapsed += TestSite::kStepMs) {
    site.runMs(TestSite::kStepMs);
    sinceLastMs += TestSite::kStepMs;
    if (site.timesTransmitted(alert) == transmissions) continue;
    transmissions = site.timesTransmitted(alert);
    spacingsMs.push_back(sinceLastMs);
    sinceLastMs = 0;
  }
  return spacingsMs;
}

std::vector<Packet> sirenNodeHeartbeats(const TestSite& site) {
  std::vector<Packet> heartbeats;
  for (const Packet& packet : site.packetsOnAir(PacketKind::Heartbeat)) {
    if (packet.sender == site.config.sirenNode.id) heartbeats.push_back(packet);
  }
  return heartbeats;
}

// How many of the siren node's heartbeats so far say that it fell silent for want
// of an alert refresh.
size_t heartbeatsReportingALostRefresh(const TestSite& site) {
  size_t count = 0;
  for (const Packet& heartbeat : sirenNodeHeartbeats(site)) {
    if (heartbeat.heartbeat.alertRefreshLost) ++count;
  }
  return count;
}

// A refresh interval well inside the siren node's timeout, for scenarios that play
// the wadi node themselves.
uint64_t refreshIntervalMs(const TestSite& site) {
  return site.config.sirenNode.alertRefreshTimeoutMs / 3;
}

// The flood is over: the bed is dry again, and the wadi node has measured it.
void waterRecedes(TestSite& site) {
  site.levelSensor.setWaterLevelCm(0);
  site.runMs(2 * site.config.wadiNode.mode.stormMeasureIntervalMs);
}

// An officer issues the all clear on the dashboard, and the gateway relays it.
void issueAllClear(TestSite& site) {
  site.issueAllClear();
  site.runMs(site.allClearSettleMs());
}

// The scenario's own wadi node re-sends its alert at every refresh interval for `durationMs`.
void keepRefreshing(TestSite& site, ScriptedNode& wadiNode, AlertLevel alertLevel,
                    uint64_t durationMs) {
  for (uint64_t elapsed = 0; elapsed < durationMs; elapsed += refreshIntervalMs(site)) {
    site.runMs(refreshIntervalMs(site));
    site.air.inject(wadiNode.alert(alertLevel));
  }
}

}  // namespace

// --- The wadi node transmits each alert three times ---

static void test_an_alert_is_transmitted_the_configured_number_of_times() {
  TestSite site;

  floodTo(site, AlertLevel::Warning);
  site.runMs(repeatWindowMs(site));

  const Frame alert = site.framesOnAir(PacketKind::Alert).front();
  TEST_ASSERT_EQUAL_UINT(site.config.wadiNode.alertRepeat.transmissions,
                         site.timesTransmitted(alert));

  // And no more after that.
  site.runMs(repeatWindowMs(site));
  TEST_ASSERT_EQUAL_UINT(site.config.wadiNode.alertRepeat.transmissions,
                         site.timesTransmitted(alert));
}

static void test_the_spacing_between_transmissions_of_an_alert_is_drawn_at_random_within_its_bounds() {
  const wadiwatch::AlertRepeatConfig repeat = wadiwatch::prototypeSite().wadiNode.alertRepeat;
  const size_t waits = repeat.transmissions - 1;
  // The scenario needs room between the bounds for a draw that is neither.
  TEST_ASSERT_TRUE(repeat.minSpacingMs + TestSite::kStepMs < repeat.maxSpacingMs);

  // Every draw the lowest: every wait is the shortest.
  std::vector<uint64_t> spacingsMs = alertSpacingsMs(std::vector<uint32_t>(waits, FakeRandom::kLowest));
  TEST_ASSERT_EQUAL_UINT(waits, spacingsMs.size());
  for (uint64_t spacingMs : spacingsMs) TEST_ASSERT_EQUAL_UINT64(repeat.minSpacingMs, spacingMs);

  // Every draw the highest: every wait is the longest.
  spacingsMs = alertSpacingsMs(std::vector<uint32_t>(waits, FakeRandom::kHighest));
  TEST_ASSERT_EQUAL_UINT(waits, spacingsMs.size());
  for (uint64_t spacingMs : spacingsMs) TEST_ASSERT_EQUAL_UINT64(repeat.maxSpacingMs, spacingMs);

  // Each wait has its own draw.
  std::vector<uint32_t> alternating;
  for (size_t i = 0; i < waits; ++i) {
    alternating.push_back(i % 2 == 0 ? FakeRandom::kHighest : FakeRandom::kLowest);
  }
  spacingsMs = alertSpacingsMs(alternating);
  TEST_ASSERT_EQUAL_UINT(waits, spacingsMs.size());
  for (size_t i = 0; i < waits; ++i) {
    TEST_ASSERT_EQUAL_UINT64(i % 2 == 0 ? repeat.maxSpacingMs : repeat.minSpacingMs, spacingsMs[i]);
  }

  // A draw in between gives a wait in between: that many milliseconds past the shortest.
  spacingsMs = alertSpacingsMs({static_cast<uint32_t>(TestSite::kStepMs)});
  TEST_ASSERT_EQUAL_UINT(waits, spacingsMs.size());
  TEST_ASSERT_EQUAL_UINT64(repeat.minSpacingMs + TestSite::kStepMs, spacingsMs[0]);
}

static void test_with_the_first_transmission_lost_the_alert_still_sounds_the_siren_on_a_repeat() {
  TestSite site;
  site.air.dropNext(1);

  floodTo(site, AlertLevel::Warning);

  // The alert has gone out once and was lost: the siren node has heard nothing.
  // Not even the reading of the same measurement: it waits behind the alert.
  TEST_ASSERT_EQUAL_UINT(1, site.air.transmitted().size());
  TEST_ASSERT_FALSE(site.siren.isOn());
  TEST_ASSERT_FALSE(site.siren.isStrobeOn());

  TEST_ASSERT_TRUE(sirenSoundsWithin(site, repeatWindowMs(site)));
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());
  TEST_ASSERT_TRUE(site.siren.isStrobeOn());
}

static void test_with_every_transmission_but_the_last_lost_the_alert_still_sounds_the_siren() {
  TestSite site;
  const wadiwatch::AlertRepeatConfig& repeat = site.config.wadiNode.alertRepeat;
  TEST_ASSERT_TRUE(repeat.transmissions > 1);

  // The first transmission is lost, and so is every further one but the last.
  site.air.dropNext(1);
  floodTo(site, AlertLevel::Evacuate);
  const Frame alert = site.framesOnAir(PacketKind::Alert).front();
  for (size_t lost = 1; lost + 1 < repeat.transmissions; ++lost) {
    loseNextTransmissionOf(site, alert);
  }
  TEST_ASSERT_EQUAL_UINT(repeat.transmissions - 1u, site.timesTransmitted(alert));
  TEST_ASSERT_EQUAL_UINT(0, site.packetsOnAir(PacketKind::Acknowledgement).size());
  TEST_ASSERT_FALSE(site.siren.isOn());

  TEST_ASSERT_TRUE(sirenSoundsWithin(site, repeat.maxSpacingMs));
  TEST_ASSERT_EQUAL_UINT(repeat.transmissions, site.timesTransmitted(alert));
  ASSERT_SIREN_SOUND(SirenSound::Continuous, site.listenToSiren());
}

// A receiver that has accepted a later packet from the wadi node refuses the alert's
// further transmissions as out of date. So nothing else may leave the wadi node
// between the first transmission of an alert and the last.
static void test_a_reading_and_a_heartbeat_wait_until_the_alert_has_had_all_its_transmissions() {
  TestSite site;
  const wadiwatch::WadiNodeConfig& config = site.config.wadiNode;
  const NodeId wadiNode = config.id;
  TEST_ASSERT_TRUE(config.alertRepeat.transmissions > 1);

  // The wadi floods just as the hourly heartbeat falls due: the measurement that
  // raises the alert has a reading to send, and the heartbeat is due with it.
  site.runMs(config.heartbeatIntervalMs - TestSite::kStepMs);
  TEST_ASSERT_EQUAL_UINT(0, site.packetsOnAirFrom(wadiNode).size());
  floodTo(site, AlertLevel::Warning);
  const Frame alert = site.framesOnAir(PacketKind::Alert).front();

  // Step by step: the alert is all the wadi node sends until it has gone out in full.
  for (uint64_t waited = 0; site.timesTransmitted(alert) < config.alertRepeat.transmissions;
       waited += TestSite::kStepMs) {
    TEST_ASSERT_TRUE_MESSAGE(waited < repeatWindowMs(site), "the alert was not repeated in time");
    TEST_ASSERT_EQUAL_UINT(1, site.packetsOnAirFrom(wadiNode).size());
    site.runMs(TestSite::kStepMs);
  }

  // What was held back follows the last transmission at once, the reading first.
  const std::vector<Packet> sent = site.packetsOnAirFrom(wadiNode);
  TEST_ASSERT_EQUAL_UINT(3, sent.size());
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PacketKind::Alert), static_cast<uint8_t>(sent[0].kind));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PacketKind::Reading), static_cast<uint8_t>(sent[1].kind));
  ASSERT_ALERT_LEVEL(AlertLevel::Warning, sent[1].reading.alertLevel);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PacketKind::Heartbeat),
                          static_cast<uint8_t>(sent[2].kind));

  // And the hourly heartbeats go on from there.
  site.runMs(config.heartbeatIntervalMs);
  size_t heartbeats = 0;
  for (const Packet& packet : site.packetsOnAirFrom(wadiNode)) {
    if (packet.kind == PacketKind::Heartbeat) ++heartbeats;
  }
  TEST_ASSERT_EQUAL_UINT(2, heartbeats);
}

static void test_a_higher_alert_level_takes_over_the_repeats_of_the_one_before() {
  TestSite site;
  const wadiwatch::WadiNodeConfig& config = site.config.wadiNode;
  // The first repeat of the Watch alert is as far off as it can be, and further
  // off than the measurement that follows.
  TEST_ASSERT_TRUE(config.alertRepeat.maxSpacingMs > TestSite::kStepMs);
  site.wadiNodeRandom.setDraws({FakeRandom::kHighest});

  // Light rain puts the node in storm mode. Just before one of its measurements
  // the rain turns heavy, which raises Watch at once; the measurement then finds
  // the wadi at Warning.
  site.rainGauge.tip();
  const size_t readsBefore = site.levelSensor.readCount();
  while (site.levelSensor.readCount() == readsBefore) site.runMs(TestSite::kStepMs);
  site.runMs(config.mode.stormMeasureIntervalMs - 2 * TestSite::kStepMs);
  site.rainGauge.rainMm(config.alertThresholds.watchRainMm);
  site.runMs(TestSite::kStepMs);
  ASSERT_ALERT_LEVEL(AlertLevel::Watch, site.latestAlertOnAir());
  site.levelSensor.setWaterLevelCm(levelWithin(site, AlertLevel::Warning));
  site.runMs(TestSite::kStepMs);
  ASSERT_ALERT_LEVEL(AlertLevel::Warning, site.latestAlertOnAir());

  site.runMs(repeatWindowMs(site));

  // The Watch alert is not repeated behind the Warning that replaced it.
  const std::vector<Frame> alerts = site.framesOnAir(PacketKind::Alert);
  TEST_ASSERT_EQUAL_UINT(2, alerts.size());
  TEST_ASSERT_EQUAL_UINT(1, site.timesTransmitted(alerts[0]));
  TEST_ASSERT_EQUAL_UINT(config.alertRepeat.transmissions, site.timesTransmitted(alerts[1]));
}

// --- The siren node acknowledges ---

static void test_the_siren_node_sends_an_acknowledgement_for_each_accepted_alert() {
  TestSite site;

  floodTo(site, AlertLevel::Watch);
  floodTo(site, AlertLevel::Warning);
  floodTo(site, AlertLevel::Evacuate);
  site.runMs(repeatWindowMs(site));

  const std::vector<Packet> alerts = site.packetsOnAir(PacketKind::Alert);
  const std::vector<Packet> acknowledgements = site.packetsOnAir(PacketKind::Acknowledgement);
  TEST_ASSERT_TRUE(alerts.size() >= 3);
  ASSERT_ALERT_LEVEL(AlertLevel::Evacuate, alerts.back().alert.alertLevel);
  // One for each alert, Watch included, naming that alert.
  TEST_ASSERT_EQUAL_UINT(alerts.size(), acknowledgements.size());
  for (size_t i = 0; i < alerts.size(); ++i) {
    const Packet& acknowledgement = acknowledgements[i];
    TEST_ASSERT_EQUAL_UINT16(site.config.sirenNode.id, acknowledgement.sender);
    TEST_ASSERT_EQUAL_UINT16(alerts[i].sender, acknowledgement.acknowledgement.alertSender);
    TEST_ASSERT_EQUAL_UINT32(alerts[i].counter, acknowledgement.acknowledgement.alertCounter);
    ASSERT_ALERT_LEVEL(alerts[i].alert.alertLevel, acknowledgement.acknowledgement.alertLevel);
  }

  // The further transmissions of an alert are not acknowledged again.
  for (const Frame& acknowledgement : site.framesOnAir(PacketKind::Acknowledgement)) {
    TEST_ASSERT_EQUAL_UINT(1, site.timesTransmitted(acknowledgement));
  }
}

static void test_an_acknowledgement_names_the_alert_level_the_siren_node_is_acting_on() {
  TestSite site;
  site.unplugWadiNode();
  ScriptedNode wadiNode(site.config.auth, site.config.wadiNode.id);
  site.air.inject(wadiNode.alert(AlertLevel::Evacuate));
  site.runMs(TestSite::kStepMs);

  // A lower alert level from the same wadi node does not quieten the siren.
  site.air.inject(wadiNode.alert(AlertLevel::Warning));
  site.runMs(TestSite::kStepMs);

  const std::vector<Packet> acknowledgements = site.packetsOnAir(PacketKind::Acknowledgement);
  TEST_ASSERT_EQUAL_UINT(2, acknowledgements.size());
  TEST_ASSERT_TRUE(acknowledgements[1].acknowledgement.alertCounter >
                   acknowledgements[0].acknowledgement.alertCounter);
  ASSERT_ALERT_LEVEL(AlertLevel::Evacuate, acknowledgements[1].acknowledgement.alertLevel);
  ASSERT_SIREN_SOUND(SirenSound::Continuous, site.listenToSiren());
}

static void test_an_alert_that_is_not_accepted_is_not_acknowledged() {
  TestSite site;
  SiteAuth forgerAuth = site.config.auth;
  forgerAuth.key[0] ^= 0x01;
  ScriptedNode forger(forgerAuth, site.config.wadiNode.id);
  ScriptedNode outsider(site.config.auth, site.nodeIdOutsideSite());

  site.air.inject(forger.alert(AlertLevel::Warning));
  site.air.inject(outsider.alert(AlertLevel::Warning));
  site.runMinutes(1);

  TEST_ASSERT_EQUAL_UINT(0, site.air.transmitted().size());
}

// --- The siren node latches the alert until an all clear ---

static void test_the_siren_stays_on_after_the_wadi_node_stops_transmitting_until_an_accepted_all_clear() {
  TestSite site;
  floodTo(site, AlertLevel::Warning);
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());

  site.unplugWadiNode();
  site.runMs(site.config.sirenNode.alertRefreshTimeoutMs / 2);
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());
  TEST_ASSERT_TRUE(site.siren.isStrobeOn());

  issueAllClear(site);

  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
  TEST_ASSERT_FALSE(site.siren.isStrobeOn());
}

static void test_an_all_clear_that_is_not_accepted_does_not_silence_the_siren() {
  TestSite site;
  floodTo(site, AlertLevel::Evacuate);
  waterRecedes(site);

  // Signed with another key, in the gateway's name.
  SiteAuth forgerAuth = site.config.auth;
  forgerAuth.key[0] ^= 0x01;
  ScriptedNode forger(forgerAuth, site.config.gateway.id);
  site.air.inject(forger.allClear(site.config.wadiNode.id));
  ASSERT_SIREN_SOUND(SirenSound::Continuous, site.listenToSiren());

  // Signed with the site key, by a transmitter that is not a node of the site.
  ScriptedNode outsider(site.config.auth, site.nodeIdOutsideSite());
  site.air.inject(outsider.allClear(site.config.wadiNode.id));
  ASSERT_SIREN_SOUND(SirenSound::Continuous, site.listenToSiren());

  // Control: the all clear the gateway transmits does.
  issueAllClear(site);
  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
}

static void test_an_all_clear_for_another_wadi_node_does_not_silence_the_siren() {
  // The second wadi node is mounted over a wadi of its own.
  wadiwatch::SiteConfig config = wadiwatch::prototypeSite();
  wadiwatch::WadiConfig& wadi = config.gateway.wadis[0];
  wadiwatch::WadiConfig otherWadi = wadi;
  otherWadi.id = static_cast<wadiwatch::WadiId>(wadi.id + 1);
  otherWadi.wadiNodes[0] = config.downstreamWadiNode.id;
  otherWadi.wadiNodeCount = 1;
  wadi.wadiNodes[0] = config.wadiNode.id;
  wadi.wadiNodeCount = 1;
  config.gateway.wadis[1] = otherWadi;
  config.gateway.wadiCount = 2;
  TestSite site(config);
  floodTo(site, AlertLevel::Warning);

  // The event at the other wadi is over; this wadi node's is not.
  site.issueAllClear(otherWadi.id);
  site.runMs(site.allClearSettleMs());

  const std::vector<Packet> allClears = site.packetsOnAir(PacketKind::AllClear);
  TEST_ASSERT_EQUAL_UINT(1, allClears.size());
  TEST_ASSERT_EQUAL_UINT16(config.downstreamWadiNode.id, allClears[0].allClear.wadiNode);
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());
  TEST_ASSERT_TRUE(site.siren.isStrobeOn());
}

static void test_after_an_all_clear_a_new_alert_sounds_the_siren_and_the_old_all_clear_cannot_silence_it() {
  TestSite site;
  floodTo(site, AlertLevel::Warning);
  waterRecedes(site);
  issueAllClear(site);
  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
  const std::vector<Frame> allClears = site.framesOnAir(PacketKind::AllClear);
  TEST_ASSERT_FALSE(allClears.empty());

  floodTo(site, AlertLevel::Evacuate);
  ASSERT_SIREN_SOUND(SirenSound::Continuous, site.listenToSiren());

  // Someone plays the recorded all clears back.
  for (const Frame& allClear : allClears) site.air.inject(allClear);

  ASSERT_SIREN_SOUND(SirenSound::Continuous, site.listenToSiren());
  TEST_ASSERT_TRUE(site.siren.isStrobeOn());
}

// --- No alert refresh ---

static void test_with_no_alert_refresh_for_the_configured_timeout_the_siren_falls_silent_and_the_heartbeat_reports_it() {
  TestSite site;
  const uint64_t timeoutMs = site.config.sirenNode.alertRefreshTimeoutMs;
  floodTo(site, AlertLevel::Warning);
  site.unplugWadiNode();

  // Up to the timeout the alert is in force and no heartbeat says otherwise.
  site.runMs(timeoutMs - 2 * patternPeriodMs(site));
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());
  TEST_ASSERT_TRUE(site.siren.isStrobeOn());
  TEST_ASSERT_EQUAL_UINT(0, heartbeatsReportingALostRefresh(site));

  site.runMs(patternPeriodMs(site));

  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
  TEST_ASSERT_FALSE(site.siren.isStrobeOn());
  // The siren node says so at once, without waiting for its next hourly heartbeat.
  const std::vector<Packet> heartbeats = sirenNodeHeartbeats(site);
  TEST_ASSERT_FALSE(heartbeats.empty());
  TEST_ASSERT_TRUE(heartbeats.back().heartbeat.alertRefreshLost);
  TEST_ASSERT_EQUAL_UINT(1, heartbeatsReportingALostRefresh(site));

  // And goes on saying so in its hourly heartbeats.
  site.runMs(site.config.sirenNode.heartbeatIntervalMs);
  TEST_ASSERT_TRUE(heartbeatsReportingALostRefresh(site) >= 2);
  TEST_ASSERT_TRUE(sirenNodeHeartbeats(site).back().heartbeat.alertRefreshLost);
  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
}

static void test_an_alert_refreshed_within_the_timeout_stays_in_force() {
  TestSite site;
  const uint64_t timeoutMs = site.config.sirenNode.alertRefreshTimeoutMs;
  // The scenario plays the wadi node, re-sending its alert the way a wadi node at
  // Warning or above does.
  site.unplugWadiNode();
  ScriptedNode wadiNode(site.config.auth, site.config.wadiNode.id);
  site.air.inject(wadiNode.alert(AlertLevel::Warning));

  keepRefreshing(site, wadiNode, AlertLevel::Warning, 2 * timeoutMs);

  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());
  TEST_ASSERT_TRUE(site.siren.isStrobeOn());

  // The refreshes stop: one timeout later the siren is silent.
  site.runMs(timeoutMs);
  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());

  // Only then does a heartbeat report a lost refresh.
  TEST_ASSERT_EQUAL_UINT(1, heartbeatsReportingALostRefresh(site));
  TEST_ASSERT_TRUE(sirenNodeHeartbeats(site).back().heartbeat.alertRefreshLost);
}

static void test_a_refresh_heard_after_the_siren_fell_silent_sounds_it_again_and_clears_the_report() {
  TestSite site;
  site.unplugWadiNode();
  ScriptedNode wadiNode(site.config.auth, site.config.wadiNode.id);
  site.air.inject(wadiNode.alert(AlertLevel::Evacuate));
  site.runMs(site.config.sirenNode.alertRefreshTimeoutMs + TestSite::kStepMs);
  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
  const size_t reportsBefore = heartbeatsReportingALostRefresh(site);
  TEST_ASSERT_EQUAL_UINT(1, reportsBefore);

  site.air.inject(wadiNode.alert(AlertLevel::Evacuate));

  ASSERT_SIREN_SOUND(SirenSound::Continuous, site.listenToSiren());
  TEST_ASSERT_TRUE(site.siren.isStrobeOn());

  // With the refreshes back, the hourly heartbeats that follow report nothing lost.
  const size_t heartbeatsBefore = sirenNodeHeartbeats(site).size();
  keepRefreshing(site, wadiNode, AlertLevel::Evacuate, site.config.sirenNode.heartbeatIntervalMs);
  TEST_ASSERT_TRUE(sirenNodeHeartbeats(site).size() > heartbeatsBefore);
  TEST_ASSERT_EQUAL_UINT(reportsBefore, heartbeatsReportingALostRefresh(site));
}

static void test_a_watch_that_is_never_refreshed_is_not_reported_as_a_lost_refresh() {
  TestSite site;
  // A wadi node at Watch sends its alert once and does not re-send it.
  floodTo(site, AlertLevel::Watch);

  site.runMs(static_cast<uint64_t>(site.config.sirenNode.alertRefreshTimeoutMs) +
             site.config.sirenNode.heartbeatIntervalMs);

  TEST_ASSERT_FALSE(sirenNodeHeartbeats(site).empty());
  TEST_ASSERT_EQUAL_UINT(0, heartbeatsReportingALostRefresh(site));
}

// --- The latched alert survives a restart ---

static void test_a_siren_node_restarted_during_an_alert_resumes_the_latched_alert() {
  const AlertLevel alertLevels[] = {AlertLevel::Warning, AlertLevel::Evacuate};
  const SirenSound sounds[] = {SirenSound::Intermittent, SirenSound::Continuous};
  for (size_t i = 0; i < 2; ++i) {
    TestSite site;
    floodTo(site, alertLevels[i]);
    ASSERT_SIREN_SOUND(sounds[i], site.listenToSiren());
    // Nothing more comes from the wadi node: only what the siren node kept can sound the siren.
    site.unplugWadiNode();

    site.restartSirenNode();

    ASSERT_SIREN_SOUND(sounds[i], site.listenToSiren());
    TEST_ASSERT_TRUE(site.siren.isStrobeOn());
  }
}

static void test_a_siren_node_restarted_after_the_all_clear_stays_silent() {
  TestSite site;
  floodTo(site, AlertLevel::Evacuate);
  waterRecedes(site);
  issueAllClear(site);
  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());

  site.restartSirenNode();

  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
  TEST_ASSERT_FALSE(site.siren.isStrobeOn());
}

static void test_an_alert_resumed_after_a_restart_still_falls_silent_when_nothing_refreshes_it() {
  TestSite site;
  floodTo(site, AlertLevel::Warning);
  site.unplugWadiNode();
  site.restartSirenNode();
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());

  // The siren node cannot know how long it was off: it waits a whole timeout from its restart.
  site.runMs(site.config.sirenNode.alertRefreshTimeoutMs);

  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
  TEST_ASSERT_EQUAL_UINT(1, heartbeatsReportingALostRefresh(site));
}

// --- The button under the cover ---

static void test_a_button_press_starts_a_test_and_emits_a_test_packet_and_a_second_press_silences_the_test() {
  TestSite site;
  // The scenario listens to the test for one pattern period before silencing it.
  TEST_ASSERT_TRUE(site.config.sirenNode.testDurationMs > 2 * patternPeriodMs(site));

  site.sirenButton.press();
  site.runMs(TestSite::kStepMs);

  TEST_ASSERT_TRUE(site.siren.isOn());
  TEST_ASSERT_TRUE(site.siren.isStrobeOn());
  std::vector<Packet> tests = site.packetsOnAir(PacketKind::Test);
  TEST_ASSERT_EQUAL_UINT(1, tests.size());
  TEST_ASSERT_EQUAL_UINT16(site.config.sirenNode.id, tests[0].sender);
  // A test is reported as a test, never as an alert.
  TEST_ASSERT_EQUAL_UINT(0, site.packetsOnAir(PacketKind::Alert).size());
  ASSERT_SIREN_SOUND(SirenSound::Continuous, site.listenToSiren());

  site.sirenButton.press();

  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
  TEST_ASSERT_FALSE(site.siren.isStrobeOn());
  TEST_ASSERT_EQUAL_UINT(1, site.packetsOnAir(PacketKind::Test).size());

  // The next press is a new test.
  site.sirenButton.press();
  site.runMs(TestSite::kStepMs);
  TEST_ASSERT_TRUE(site.siren.isOn());
  TEST_ASSERT_EQUAL_UINT(2, site.packetsOnAir(PacketKind::Test).size());
}

static void test_a_test_ends_by_itself_after_the_configured_duration() {
  TestSite site;
  site.sirenButton.press();
  site.runMs(TestSite::kStepMs);

  site.runMs(site.config.sirenNode.testDurationMs - TestSite::kStepMs);
  TEST_ASSERT_TRUE(site.siren.isOn());
  TEST_ASSERT_TRUE(site.siren.isStrobeOn());

  site.runMs(TestSite::kStepMs);
  TEST_ASSERT_FALSE(site.siren.isOn());
  TEST_ASSERT_FALSE(site.siren.isStrobeOn());

  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
  TEST_ASSERT_EQUAL_UINT(1, site.packetsOnAir(PacketKind::Test).size());
}

static void test_a_button_press_during_a_real_alert_does_not_silence_it() {
  const AlertLevel alertLevels[] = {AlertLevel::Warning, AlertLevel::Evacuate};
  const SirenSound sounds[] = {SirenSound::Intermittent, SirenSound::Continuous};
  for (size_t i = 0; i < 2; ++i) {
    TestSite site;
    floodTo(site, alertLevels[i]);
    ASSERT_SIREN_SOUND(sounds[i], site.listenToSiren());

    // However many times the button is pressed.
    for (int press = 0; press < 3; ++press) {
      site.sirenButton.press();
      ASSERT_SIREN_SOUND(sounds[i], site.listenToSiren());
      TEST_ASSERT_TRUE(site.siren.isStrobeOn());
    }

    // And no test is reported in the middle of the event.
    TEST_ASSERT_EQUAL_UINT(0, site.packetsOnAir(PacketKind::Test).size());
  }
}

static void test_an_alert_that_arrives_during_a_test_takes_over_and_the_button_cannot_silence_it() {
  // A gateway that transmits the all clear as soon as the command arrives, so that the
  // scenario is over before the test would have ended by itself.
  wadiwatch::SiteConfig config = wadiwatch::prototypeSite();
  config.gateway.allClearHoldMs = 0;
  TestSite site(config);
  TEST_ASSERT_TRUE(site.config.sirenNode.testDurationMs > 4 * patternPeriodMs(site));
  site.unplugWadiNode();
  ScriptedNode wadiNode(site.config.auth, site.config.wadiNode.id);
  site.sirenButton.press();
  site.runMs(TestSite::kStepMs);
  TEST_ASSERT_TRUE(site.siren.isOn());

  site.air.inject(wadiNode.alert(AlertLevel::Warning));

  // The siren changes from the test to the Warning pattern.
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());

  // What would have silenced the test does nothing to the alert.
  site.sirenButton.press();
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());
  TEST_ASSERT_TRUE(site.siren.isStrobeOn());

  // After the all clear the interrupted test does not start up again. The gateway
  // takes the command in and transmits in one step; the siren node hears in the next.
  site.issueAllClear();
  site.runMs(2 * TestSite::kStepMs);
  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
  TEST_ASSERT_FALSE(site.siren.isStrobeOn());
}

// --- The siren node's heartbeat ---

static void test_the_siren_node_sends_an_hourly_heartbeat_with_battery_voltage_and_signal_strength() {
  TestSite site;
  const uint64_t minuteMs = 60 * 1000;
  const uint64_t intervalMinutes = site.config.sirenNode.heartbeatIntervalMs / minuteMs;
  const uint16_t batteryMillivolts = 12650;
  const int8_t signalStrengthDbm = -97;
  site.sirenNodeHealth.setBatteryMillivolts(batteryMillivolts);
  site.sirenNodeHealth.setSignalStrengthDbm(signalStrengthDbm);
  site.sirenNodeHealth.setTamper(true);
  // The wadi node's own health is not what the siren node reports.
  site.wadiNodeHealth.setBatteryMillivolts(3920);
  site.wadiNodeHealth.setSignalStrengthDbm(-104);

  // Three hours and a minute, watched minute by minute.
  const uint64_t hours = 3;
  std::vector<uint64_t> heartbeatMinutes;
  for (uint64_t minute = 0; minute <= hours * intervalMinutes; ++minute) {
    const size_t before = sirenNodeHeartbeats(site).size();
    site.runMinutes(1);
    if (sirenNodeHeartbeats(site).size() > before) heartbeatMinutes.push_back(minute);
  }

  // The first comes one interval after the node starts, then one per interval.
  TEST_ASSERT_EQUAL_UINT(hours, heartbeatMinutes.size());
  for (size_t i = 0; i < heartbeatMinutes.size(); ++i) {
    TEST_ASSERT_EQUAL_UINT64((i + 1) * intervalMinutes, heartbeatMinutes[i]);
  }

  const std::vector<Packet> heartbeats = sirenNodeHeartbeats(site);
  TEST_ASSERT_EQUAL_UINT(hours, heartbeats.size());
  for (const Packet& heartbeat : heartbeats) {
    TEST_ASSERT_EQUAL_UINT16(batteryMillivolts, heartbeat.heartbeat.batteryMillivolts);
    TEST_ASSERT_EQUAL_INT16(signalStrengthDbm, heartbeat.heartbeat.signalStrengthDbm);
    TEST_ASSERT_TRUE(heartbeat.heartbeat.tamper);
    // A siren node measures no level and, with no alert, has lost no refresh.
    TEST_ASSERT_FALSE(heartbeat.heartbeat.hasLastLevel);
    TEST_ASSERT_FALSE(heartbeat.heartbeat.alertRefreshLost);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_an_alert_is_transmitted_the_configured_number_of_times);
  RUN_TEST(test_the_spacing_between_transmissions_of_an_alert_is_drawn_at_random_within_its_bounds);
  RUN_TEST(test_with_the_first_transmission_lost_the_alert_still_sounds_the_siren_on_a_repeat);
  RUN_TEST(test_with_every_transmission_but_the_last_lost_the_alert_still_sounds_the_siren);
  RUN_TEST(test_a_reading_and_a_heartbeat_wait_until_the_alert_has_had_all_its_transmissions);
  RUN_TEST(test_a_higher_alert_level_takes_over_the_repeats_of_the_one_before);
  RUN_TEST(test_the_siren_node_sends_an_acknowledgement_for_each_accepted_alert);
  RUN_TEST(test_an_acknowledgement_names_the_alert_level_the_siren_node_is_acting_on);
  RUN_TEST(test_an_alert_that_is_not_accepted_is_not_acknowledged);
  RUN_TEST(test_the_siren_stays_on_after_the_wadi_node_stops_transmitting_until_an_accepted_all_clear);
  RUN_TEST(test_an_all_clear_that_is_not_accepted_does_not_silence_the_siren);
  RUN_TEST(test_an_all_clear_for_another_wadi_node_does_not_silence_the_siren);
  RUN_TEST(test_after_an_all_clear_a_new_alert_sounds_the_siren_and_the_old_all_clear_cannot_silence_it);
  RUN_TEST(test_with_no_alert_refresh_for_the_configured_timeout_the_siren_falls_silent_and_the_heartbeat_reports_it);
  RUN_TEST(test_an_alert_refreshed_within_the_timeout_stays_in_force);
  RUN_TEST(test_a_refresh_heard_after_the_siren_fell_silent_sounds_it_again_and_clears_the_report);
  RUN_TEST(test_a_watch_that_is_never_refreshed_is_not_reported_as_a_lost_refresh);
  RUN_TEST(test_a_siren_node_restarted_during_an_alert_resumes_the_latched_alert);
  RUN_TEST(test_a_siren_node_restarted_after_the_all_clear_stays_silent);
  RUN_TEST(test_an_alert_resumed_after_a_restart_still_falls_silent_when_nothing_refreshes_it);
  RUN_TEST(test_a_button_press_starts_a_test_and_emits_a_test_packet_and_a_second_press_silences_the_test);
  RUN_TEST(test_a_test_ends_by_itself_after_the_configured_duration);
  RUN_TEST(test_a_button_press_during_a_real_alert_does_not_silence_it);
  RUN_TEST(test_an_alert_that_arrives_during_a_test_takes_over_and_the_button_cannot_silence_it);
  RUN_TEST(test_the_siren_node_sends_an_hourly_heartbeat_with_battery_voltage_and_signal_strength);
  return UNITY_END();
}
