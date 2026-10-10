// Scenario tests for upstream confirmation: two wadi nodes on one wadi. A node at
// Warning or above sends its alert again every re-send interval; the node downstream
// listens for it, and Warning at both raises Evacuate downstream.
//
// Every duration comes from the site configuration. The scenarios step the site one
// simulation step at a time, so they assume the configured durations are whole steps.
#include <cmath>
#include <cstdint>
#include <vector>

#include <unity.h>

#include "wadiwatch/alert_engine.h"
#include "wadiwatch/packet.h"
#include "wadiwatch/radio_config.h"
#include "wadiwatch_testing/test_site.h"

using wadiwatch::AlertLevel;
using wadiwatch::kNoNode;
using wadiwatch::levelThresholdCm;
using wadiwatch::NodeId;
using wadiwatch::Packet;
using wadiwatch::PacketCodec;
using wadiwatch::PacketKind;
using wadiwatch::SiteAuth;
using wadiwatch::SiteConfig;
using wadiwatch::WadiNodeConfig;
using wadiwatch::testing::FakeLevelSensor;
using wadiwatch::testing::FakeRainGauge;
using wadiwatch::testing::FakeSleepControl;
using wadiwatch::testing::FakeStorage;
using wadiwatch::testing::Frame;
using wadiwatch::testing::InMemoryRadio;
using wadiwatch::testing::SirenSound;
using wadiwatch::testing::TestSite;

void setUp() {}
void tearDown() {}

#define ASSERT_ALERT_LEVEL(expected, actual) \
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected), static_cast<uint8_t>(actual))

#define ASSERT_SIREN_SOUND(expected, actual) \
  TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual))

namespace {

const uint64_t kStepMs = TestSite::kStepMs;

// One of the site's two wadi nodes, with the hardware a scenario touches.
struct Node {
  const WadiNodeConfig& config;
  FakeLevelSensor& levelSensor;
  FakeRainGauge& rainGauge;
  FakeSleepControl& sleep;
  InMemoryRadio& radio;
};

Node firstNode(TestSite& site) {
  return Node{site.config.wadiNode, site.levelSensor, site.rainGauge, site.wadiNodeSleep,
              site.wadiNodeRadio};
}

Node secondNode(TestSite& site) {
  return Node{site.config.downstreamWadiNode, site.downstream.levelSensor,
              site.downstream.rainGauge, site.downstream.sleep, site.downstream.radio};
}

// In the prototype site the first wadi node is upstream of the second.
Node upstreamNode(TestSite& site) { return firstNode(site); }
Node downstreamNode(TestSite& site) { return secondNode(site); }

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

float levelBelowWatch(const Node& node) {
  return levelThresholdCm(node.config.alertThresholds, AlertLevel::Watch) / 2.0f;
}

// A water level as packets carry it: whole millimetres.
uint16_t levelMm(float levelCm) { return static_cast<uint16_t>(std::lround(levelCm * 10.0f)); }

std::vector<Packet> alertsFrom(const TestSite& site, const Node& node) {
  return site.packetsOnAir(PacketKind::Alert, node.config.id);
}

// The alert level a node last announced.
AlertLevel alertLevelOf(const TestSite& site, const Node& node) {
  return site.latestAlertOnAir(node.config.id);
}

// Holds the water at one level at a node until that node has measured it,
// whichever operating mode it is in: until its next ping of the level sensor.
void holdLevel(TestSite& site, const Node& node, float levelCm) {
  node.levelSensor.setWaterLevelCm(levelCm);
  const size_t readsBefore = node.levelSensor.readCount();
  for (uint64_t waited = 0; node.levelSensor.readCount() == readsBefore; waited += kStepMs) {
    TEST_ASSERT_TRUE_MESSAGE(waited < node.config.mode.dryMeasureIntervalMs,
                             "the wadi node never measured the level");
    site.runMs(kStepMs);
  }
}

// Runs the site until `node` announces `alertLevel`, for at most `withinMs`.
// Returns how long that took; fails the test if it did not happen.
uint64_t waitForAlertLevel(TestSite& site, const Node& node, AlertLevel alertLevel,
                           uint64_t withinMs) {
  uint64_t waited = 0;
  while (alertLevelOf(site, node) != alertLevel) {
    TEST_ASSERT_TRUE_MESSAGE(waited < withinMs, "the alert level was not announced in time");
    site.runMs(kStepMs);
    waited += kStepMs;
  }
  return waited;
}

// Runs the site up to the step in which `node` next puts an alert on the air.
void waitForNextAlertFrom(TestSite& site, const Node& node) {
  const size_t alertsBefore = alertsFrom(site, node).size();
  for (uint64_t waited = 0; alertsFrom(site, node).size() == alertsBefore; waited += kStepMs) {
    TEST_ASSERT_TRUE_MESSAGE(waited < node.config.mode.dryMeasureIntervalMs,
                             "the wadi node sent no alert");
    site.runMs(kStepMs);
  }
}

// It starts to rain at the downstream node one step before a re-send of the upstream
// node, which is at Warning or above. Rain puts the downstream node in storm mode and
// makes it measure at once; from then on each of its measurements comes one step
// before a re-send, so each re-send falls inside its receive window.
void rainStartsDownstreamJustBeforeAnUpstreamResend(TestSite& site) {
  const Node upstream = upstreamNode(site);
  const Node downstream = downstreamNode(site);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(upstream.config.alertResendIntervalMs,
                                   downstream.config.mode.stormMeasureIntervalMs,
                                   "this scenario needs measurements and re-sends in step");
  TEST_ASSERT_TRUE(downstream.config.upstream.receiveWindowMs > kStepMs);

  waitForNextAlertFrom(site, upstream);
  site.runMs(upstream.config.alertResendIntervalMs - 2 * kStepMs);
  downstream.rainGauge.tip();
  site.runMs(kStepMs);
}

// What a third radio would transmit: an alert in the name of `claimedSender`, signed
// with whatever key the transmitter holds. It is that transmitter's first packet.
Frame signedAlert(const SiteAuth& auth, NodeId claimedSender, AlertLevel alertLevel) {
  FakeStorage storage;
  PacketCodec codec(auth, claimedSender, storage);
  Packet packet = Packet();
  packet.kind = PacketKind::Alert;
  packet.alert.alertLevel = alertLevel;
  uint8_t buffer[wadiwatch::kMaxPacketSize];
  const size_t length = codec.encode(packet, buffer, sizeof(buffer));
  return Frame(buffer, buffer + length);
}

// Adds one more node to the site, such as a wadi node on another wadi, and returns its identifier.
NodeId addNodeToSite(SiteConfig& config) {
  TEST_ASSERT_TRUE(config.auth.nodeCount < wadiwatch::kMaxSiteNodes);
  NodeId highest = 0;
  for (size_t i = 0; i < config.auth.nodeCount; ++i) {
    if (config.auth.nodes[i] > highest) highest = config.auth.nodes[i];
  }
  const NodeId added = static_cast<NodeId>(highest + 1);
  config.auth.nodes[config.auth.nodeCount++] = added;
  return added;
}

// How many of the next `steps` simulation steps end with the node's receiver on.
uint64_t stepsListening(TestSite& site, const Node& node, uint64_t steps) {
  uint64_t listening = 0;
  for (uint64_t step = 0; step < steps; ++step) {
    site.runMs(kStepMs);
    if (node.radio.isListening()) ++listening;
  }
  return listening;
}

// Time on air, in microseconds, of the frames a radio sent from the `firstFrame`-th on.
uint64_t timeOnAirUs(const TestSite& site, const InMemoryRadio& radio, size_t firstFrame) {
  uint64_t totalUs = 0;
  for (size_t i = firstFrame; i < radio.sent().size(); ++i) {
    totalUs += wadiwatch::loraTimeOnAirUs(site.config.radio, radio.sent()[i].size());
  }
  return totalUs;
}

}  // namespace

// --- Upstream confirmation ---

static void test_warning_at_the_upstream_node_and_at_the_downstream_node_produce_evacuate_downstream() {
  TestSite site;
  const Node upstream = upstreamNode(site);
  const Node downstream = downstreamNode(site);

  // The flood reaches the upstream node first.
  holdLevel(site, upstream, levelWithin(upstream, AlertLevel::Warning));
  ASSERT_ALERT_LEVEL(AlertLevel::Warning, alertLevelOf(site, upstream));
  ASSERT_ALERT_LEVEL(AlertLevel::None, alertLevelOf(site, downstream));
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());

  // Then it reaches the downstream node, still short of its Evacuate threshold.
  holdLevel(site, downstream, levelWithin(downstream, AlertLevel::Warning));
  waitForAlertLevel(site, downstream, AlertLevel::Evacuate,
                    upstream.config.alertResendIntervalMs + kStepMs);

  // The upstream node itself is still at Warning; the siren follows the higher alert.
  ASSERT_ALERT_LEVEL(AlertLevel::Warning, alertLevelOf(site, upstream));
  ASSERT_SIREN_SOUND(SirenSound::Continuous, site.listenToSiren());
  TEST_ASSERT_TRUE(site.siren.isStrobeOn());
}

static void test_evacuate_follows_within_one_resend_interval_whatever_the_offset_between_the_nodes() {
  const uint64_t resendIntervalMs = wadiwatch::prototypeSite().wadiNode.alertResendIntervalMs;

  // The two nodes do not share a clock: try every offset between the upstream
  // node's re-sends and the downstream node's measurements.
  for (uint64_t offsetMs = 0; offsetMs < resendIntervalMs; offsetMs += kStepMs) {
    TestSite site;
    const Node upstream = upstreamNode(site);
    const Node downstream = downstreamNode(site);
    holdLevel(site, upstream, levelWithin(upstream, AlertLevel::Warning));
    site.runMs(offsetMs);

    // Rain and the flood reach the downstream node together: it measures at once.
    downstream.levelSensor.setWaterLevelCm(levelWithin(downstream, AlertLevel::Warning));
    downstream.rainGauge.tip();

    waitForAlertLevel(site, downstream, AlertLevel::Evacuate, resendIntervalMs + kStepMs);
  }
}

static void test_an_upstream_warning_heard_in_a_receive_window_is_remembered_until_the_node_reaches_warning() {
  TestSite site;
  const Node upstream = upstreamNode(site);
  const Node downstream = downstreamNode(site);
  holdLevel(site, upstream, levelWithin(upstream, AlertLevel::Warning));
  downstream.levelSensor.setWaterLevelCm(levelBelowWatch(downstream));
  rainStartsDownstreamJustBeforeAnUpstreamResend(site);
  site.runMs(2ull * downstream.config.mode.stormMeasureIntervalMs);
  TEST_ASSERT_EQUAL_UINT(0, alertsFrom(site, downstream).size());

  // The flood arrives: the first measurement at Warning raises Evacuate at once,
  // without waiting for the upstream node to be heard again.
  holdLevel(site, downstream, levelWithin(downstream, AlertLevel::Warning));

  const std::vector<Packet> alerts = alertsFrom(site, downstream);
  TEST_ASSERT_EQUAL_UINT(1, alerts.size());
  ASSERT_ALERT_LEVEL(AlertLevel::Evacuate, alerts[0].alert.alertLevel);
}

static void test_warning_at_the_upstream_node_alone_raises_no_alert_at_a_downstream_node_in_dry_mode() {
  TestSite site;
  const Node upstream = upstreamNode(site);
  const Node downstream = downstreamNode(site);
  downstream.levelSensor.setWaterLevelCm(levelBelowWatch(downstream));

  holdLevel(site, upstream, levelWithin(upstream, AlertLevel::Warning));
  site.runMs(4ull * downstream.config.mode.dryMeasureIntervalMs);

  ASSERT_ALERT_LEVEL(AlertLevel::Warning, alertLevelOf(site, upstream));
  TEST_ASSERT_EQUAL_UINT(0, alertsFrom(site, downstream).size());
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());
}

static void test_warning_at_the_upstream_node_alone_raises_no_alert_at_a_downstream_node_that_hears_it() {
  TestSite site;
  const Node upstream = upstreamNode(site);
  const Node downstream = downstreamNode(site);
  downstream.levelSensor.setWaterLevelCm(levelBelowWatch(downstream));
  holdLevel(site, upstream, levelWithin(upstream, AlertLevel::Warning));

  // The downstream node is in storm mode and hears every re-send, for as long as
  // storm mode lasts and beyond.
  rainStartsDownstreamJustBeforeAnUpstreamResend(site);
  site.runMs(2ull * downstream.config.mode.stormQuietMs);

  TEST_ASSERT_EQUAL_UINT(0, alertsFrom(site, downstream).size());
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());
}

static void test_warning_at_the_upstream_node_does_not_raise_a_downstream_watch() {
  TestSite site;
  const Node upstream = upstreamNode(site);
  const Node downstream = downstreamNode(site);
  downstream.levelSensor.setWaterLevelCm(levelBelowWatch(downstream));
  holdLevel(site, upstream, levelWithin(upstream, AlertLevel::Warning));
  rainStartsDownstreamJustBeforeAnUpstreamResend(site);

  holdLevel(site, downstream, levelWithin(downstream, AlertLevel::Watch));
  site.runMs(10ull * downstream.config.mode.stormMeasureIntervalMs);

  const std::vector<Packet> alerts = alertsFrom(site, downstream);
  TEST_ASSERT_EQUAL_UINT(1, alerts.size());
  ASSERT_ALERT_LEVEL(AlertLevel::Watch, alerts[0].alert.alertLevel);
}

static void test_watch_at_the_upstream_node_does_not_confirm_a_downstream_warning() {
  TestSite site;
  const Node upstream = upstreamNode(site);
  const Node downstream = downstreamNode(site);
  holdLevel(site, downstream, levelWithin(downstream, AlertLevel::Warning));

  holdLevel(site, upstream, levelWithin(upstream, AlertLevel::Watch));
  ASSERT_ALERT_LEVEL(AlertLevel::Watch, alertLevelOf(site, upstream));
  site.runMs(3ull * downstream.config.mode.stormMeasureIntervalMs);
  ASSERT_ALERT_LEVEL(AlertLevel::Warning, alertLevelOf(site, downstream));

  // The downstream node was listening all along: Warning upstream does confirm.
  holdLevel(site, upstream, levelWithin(upstream, AlertLevel::Warning));
  waitForAlertLevel(site, downstream, AlertLevel::Evacuate, kStepMs);
}

static void test_a_warning_still_in_force_downstream_is_confirmed_after_its_level_has_fallen() {
  TestSite site;
  const Node upstream = upstreamNode(site);
  const Node downstream = downstreamNode(site);
  holdLevel(site, downstream, levelWithin(downstream, AlertLevel::Warning));

  // The water falls again at the downstream node. Its Warning stays in force: only
  // an all clear lowers it.
  holdLevel(site, downstream, levelBelowWatch(downstream));
  site.runMs(3ull * downstream.config.mode.stormMeasureIntervalMs);
  ASSERT_ALERT_LEVEL(AlertLevel::Warning, alertLevelOf(site, downstream));

  // A node at Warning that hears Warning from its upstream node raises Evacuate.
  holdLevel(site, upstream, levelWithin(upstream, AlertLevel::Warning));
  waitForAlertLevel(site, downstream, AlertLevel::Evacuate, kStepMs);
}

static void test_a_frame_that_arrived_before_the_receiver_was_switched_on_is_not_acted_on() {
  TestSite site;
  const Node upstream = upstreamNode(site);
  const Node downstream = downstreamNode(site);

  // The radio listens from power-on, before the node has started and switched it off.
  site.air.inject(signedAlert(site.config.auth, upstream.config.id, AlertLevel::Warning));
  holdLevel(site, downstream, levelWithin(downstream, AlertLevel::Warning));
  site.runMs(3ull * downstream.config.mode.stormMeasureIntervalMs);

  // Only what arrives while the node is listening counts as heard.
  ASSERT_ALERT_LEVEL(AlertLevel::Warning, alertLevelOf(site, downstream));
}

static void test_warning_heard_from_a_node_that_is_not_the_upstream_node_has_no_effect() {
  SiteConfig config = wadiwatch::prototypeSite();
  const NodeId otherNode = addNodeToSite(config);
  TestSite site(config);
  const Node upstream = upstreamNode(site);
  const Node downstream = downstreamNode(site);
  holdLevel(site, downstream, levelWithin(downstream, AlertLevel::Warning));
  ASSERT_ALERT_LEVEL(AlertLevel::Warning, alertLevelOf(site, downstream));

  // Another node of the site, which the downstream node accepts packets from, is at Warning.
  site.air.inject(signedAlert(site.config.auth, otherNode, AlertLevel::Warning));
  site.runMs(3ull * downstream.config.mode.stormMeasureIntervalMs);
  ASSERT_ALERT_LEVEL(AlertLevel::Warning, alertLevelOf(site, downstream));

  // The same alert in the name of the upstream node is acted on.
  site.air.inject(signedAlert(site.config.auth, upstream.config.id, AlertLevel::Warning));
  waitForAlertLevel(site, downstream, AlertLevel::Evacuate, kStepMs);
}

static void test_a_forged_upstream_warning_does_not_raise_evacuate() {
  TestSite site;
  const Node upstream = upstreamNode(site);
  const Node downstream = downstreamNode(site);
  SiteAuth forgerAuth = site.config.auth;
  forgerAuth.key[0] ^= 0x01;
  holdLevel(site, downstream, levelWithin(downstream, AlertLevel::Warning));

  site.air.inject(signedAlert(forgerAuth, upstream.config.id, AlertLevel::Warning));
  site.runMs(3ull * downstream.config.mode.stormMeasureIntervalMs);

  ASSERT_ALERT_LEVEL(AlertLevel::Warning, alertLevelOf(site, downstream));
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());
}

static void test_warning_at_the_downstream_node_does_not_raise_evacuate_at_the_upstream_node() {
  TestSite site;
  const Node upstream = upstreamNode(site);
  const Node downstream = downstreamNode(site);

  holdLevel(site, downstream, levelWithin(downstream, AlertLevel::Warning));
  holdLevel(site, upstream, levelWithin(upstream, AlertLevel::Warning));
  site.runMs(5ull * upstream.config.alertResendIntervalMs);

  ASSERT_ALERT_LEVEL(AlertLevel::Warning, alertLevelOf(site, upstream));
  ASSERT_ALERT_LEVEL(AlertLevel::Evacuate, alertLevelOf(site, downstream));
}

static void test_which_node_is_upstream_of_which_comes_from_the_site_configuration() {
  // The same two nodes with the relationship turned round: the second is now upstream of the first.
  SiteConfig config = wadiwatch::prototypeSite();
  config.wadiNode.upstream.node = config.downstreamWadiNode.id;
  config.downstreamWadiNode.upstream.node = kNoNode;
  TestSite site(config);
  const Node first = firstNode(site);
  const Node second = secondNode(site);

  holdLevel(site, second, levelWithin(second, AlertLevel::Warning));
  holdLevel(site, first, levelWithin(first, AlertLevel::Warning));
  site.runMs(5ull * second.config.alertResendIntervalMs);

  ASSERT_ALERT_LEVEL(AlertLevel::Evacuate, alertLevelOf(site, first));
  ASSERT_ALERT_LEVEL(AlertLevel::Warning, alertLevelOf(site, second));
}

static void test_without_an_upstream_relationship_warning_at_both_nodes_stays_warning() {
  SiteConfig config = wadiwatch::prototypeSite();
  config.wadiNode.upstream.node = kNoNode;
  config.downstreamWadiNode.upstream.node = kNoNode;
  TestSite site(config);
  const Node first = firstNode(site);
  const Node second = secondNode(site);

  holdLevel(site, first, levelWithin(first, AlertLevel::Warning));
  holdLevel(site, second, levelWithin(second, AlertLevel::Warning));
  site.runMs(5ull * first.config.alertResendIntervalMs);

  ASSERT_ALERT_LEVEL(AlertLevel::Warning, alertLevelOf(site, first));
  ASSERT_ALERT_LEVEL(AlertLevel::Warning, alertLevelOf(site, second));
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());
}

// --- Re-sending the alert ---

// At `alertLevel`, the node sends its alert once per re-send interval, on the interval.
static void assertAlertIsSentAgainEveryResendInterval(AlertLevel alertLevel) {
  TestSite site;
  const Node node = upstreamNode(site);
  const uint64_t resendIntervalMs = node.config.alertResendIntervalMs;
  holdLevel(site, node, levelWithin(node, alertLevel));
  TEST_ASSERT_EQUAL_UINT(1, alertsFrom(site, node).size());

  const size_t intervals = 10;
  for (size_t interval = 1; interval <= intervals; ++interval) {
    site.runMs(resendIntervalMs - kStepMs);
    TEST_ASSERT_EQUAL_UINT(interval, alertsFrom(site, node).size());
    site.runMs(kStepMs);
    TEST_ASSERT_EQUAL_UINT(interval + 1, alertsFrom(site, node).size());
  }

  // Each one is a new packet with the same alert level, so receivers take it as a refresh.
  const std::vector<Packet> alerts = alertsFrom(site, node);
  for (size_t i = 0; i < alerts.size(); ++i) {
    ASSERT_ALERT_LEVEL(alertLevel, alerts[i].alert.alertLevel);
    if (i > 0) TEST_ASSERT_TRUE(alerts[i].counter > alerts[i - 1].counter);
  }
}

static void test_a_node_at_warning_sends_its_alert_again_every_resend_interval() {
  assertAlertIsSentAgainEveryResendInterval(AlertLevel::Warning);
}

static void test_a_node_at_evacuate_sends_its_alert_again_every_resend_interval() {
  assertAlertIsSentAgainEveryResendInterval(AlertLevel::Evacuate);
}

static void test_a_node_at_watch_does_not_send_its_alert_again() {
  TestSite site;
  const Node node = upstreamNode(site);
  holdLevel(site, node, levelWithin(node, AlertLevel::Watch));

  site.runMs(10ull * node.config.alertResendIntervalMs);

  TEST_ASSERT_EQUAL_UINT(1, alertsFrom(site, node).size());
}

static void test_an_alert_sent_again_carries_the_latest_measured_level() {
  TestSite site;
  const Node node = upstreamNode(site);
  const float firstCm = levelWithin(node, AlertLevel::Warning);
  const float laterCm = levelThresholdCm(node.config.alertThresholds, AlertLevel::Warning);
  holdLevel(site, node, firstCm);

  holdLevel(site, node, laterCm);
  site.runMs(node.config.alertResendIntervalMs);

  const std::vector<Packet> alerts = alertsFrom(site, node);
  TEST_ASSERT_TRUE(alerts.size() >= 2);
  TEST_ASSERT_UINT16_WITHIN(1, levelMm(firstCm), alerts.front().alert.levelMm);
  TEST_ASSERT_UINT16_WITHIN(1, levelMm(laterCm), alerts.back().alert.levelMm);
}

static void test_escalating_sends_the_higher_alert_at_once_and_then_every_resend_interval() {
  TestSite site;
  const Node node = upstreamNode(site);
  const uint64_t resendIntervalMs = node.config.alertResendIntervalMs;
  holdLevel(site, node, levelWithin(node, AlertLevel::Warning));
  site.runMs(resendIntervalMs / 2);

  holdLevel(site, node, levelWithin(node, AlertLevel::Evacuate));
  ASSERT_ALERT_LEVEL(AlertLevel::Evacuate, alertLevelOf(site, node));
  // One Warning and one Evacuate so far: escalating is not followed by a second copy.
  const size_t alertsAtEvacuate = alertsFrom(site, node).size();
  TEST_ASSERT_EQUAL_UINT(2, alertsAtEvacuate);

  // The next one comes a full interval after the escalation.
  site.runMs(resendIntervalMs - kStepMs);
  TEST_ASSERT_EQUAL_UINT(alertsAtEvacuate, alertsFrom(site, node).size());
  site.runMs(kStepMs);
  TEST_ASSERT_EQUAL_UINT(alertsAtEvacuate + 1, alertsFrom(site, node).size());
  ASSERT_ALERT_LEVEL(AlertLevel::Evacuate, alertLevelOf(site, node));
}

static void test_an_alert_raised_between_two_measurements_is_sent_again_one_resend_interval_later() {
  TestSite site;
  const Node upstream = upstreamNode(site);
  const Node downstream = downstreamNode(site);
  const uint64_t resendIntervalMs = downstream.config.alertResendIntervalMs;
  holdLevel(site, downstream, levelWithin(downstream, AlertLevel::Warning));

  // Half-way to the downstream node's next measurement, its upstream node is heard at Warning.
  site.runMs(downstream.config.mode.stormMeasureIntervalMs / 2);
  site.air.inject(signedAlert(site.config.auth, upstream.config.id, AlertLevel::Warning));
  site.runMs(kStepMs);
  ASSERT_ALERT_LEVEL(AlertLevel::Evacuate, alertLevelOf(site, downstream));
  const size_t alertsAtEvacuate = alertsFrom(site, downstream).size();

  // The node wakes for the re-send on its own, not only at its next measurement.
  site.runMs(resendIntervalMs - kStepMs);
  TEST_ASSERT_EQUAL_UINT(alertsAtEvacuate, alertsFrom(site, downstream).size());
  site.runMs(kStepMs);
  TEST_ASSERT_EQUAL_UINT(alertsAtEvacuate + 1, alertsFrom(site, downstream).size());
}

static void test_a_node_at_warning_stays_in_storm_mode_after_the_level_has_fallen() {
  TestSite site;
  const Node node = upstreamNode(site);
  const wadiwatch::OperatingModeConfig& mode = node.config.mode;
  holdLevel(site, node, levelWithin(node, AlertLevel::Warning));

  // The flood passes and the wadi stays dry for far longer than the quiet period
  // that would take a node with no alert back to dry mode.
  node.levelSensor.setWaterLevelCm(0.0f);
  site.runMs(static_cast<uint64_t>(mode.stormQuietMs) + 2ull * mode.dryMeasureIntervalMs);

  // It still measures every storm-mode interval and still sends its alert again.
  const size_t readsBefore = node.levelSensor.readCount();
  const size_t alertsBefore = alertsFrom(site, node).size();
  site.runMs(mode.dryMeasureIntervalMs);
  TEST_ASSERT_EQUAL_UINT(mode.dryMeasureIntervalMs / mode.stormMeasureIntervalMs,
                         node.levelSensor.readCount() - readsBefore);
  TEST_ASSERT_EQUAL_UINT(mode.dryMeasureIntervalMs / node.config.alertResendIntervalMs,
                         alertsFrom(site, node).size() - alertsBefore);
  ASSERT_ALERT_LEVEL(AlertLevel::Warning, alertLevelOf(site, node));
}

static void test_alerts_sent_again_do_not_restart_the_siren_pattern() {
  TestSite site;
  const Node node = upstreamNode(site);
  const uint64_t onMs = site.config.sirenNode.intermittentOnMs;
  const uint64_t periodMs = onMs + site.config.sirenNode.intermittentOffMs;
  node.levelSensor.setWaterLevelCm(levelWithin(node, AlertLevel::Warning));
  for (uint64_t waited = 0; !site.siren.isOn(); waited += kStepMs) {
    TEST_ASSERT_TRUE_MESSAGE(waited < node.config.mode.dryMeasureIntervalMs,
                             "the siren never sounded");
    site.runMs(kStepMs);
  }

  // The pattern keeps time from the first alert, through several re-sends.
  const uint64_t watchedMs = 3ull * node.config.alertResendIntervalMs + periodMs;
  for (uint64_t elapsed = kStepMs; elapsed < watchedMs; elapsed += kStepMs) {
    site.runMs(kStepMs);
    TEST_ASSERT_EQUAL((elapsed % periodMs) < onMs, site.siren.isOn());
    TEST_ASSERT_TRUE(site.siren.isStrobeOn());
  }
  TEST_ASSERT_TRUE(alertsFrom(site, node).size() > 3);
}

// --- The receive window ---

static void test_in_dry_mode_neither_wadi_node_switches_its_receiver_on() {
  TestSite site;
  const Node upstream = upstreamNode(site);
  const Node downstream = downstreamNode(site);
  const uint64_t steps = 4ull * downstream.config.mode.dryMeasureIntervalMs / kStepMs;

  for (uint64_t step = 0; step < steps; ++step) {
    site.runMs(kStepMs);
    TEST_ASSERT_FALSE(upstream.radio.isListening());
    TEST_ASSERT_FALSE(downstream.radio.isListening());
  }
}

static void test_in_storm_mode_the_downstream_node_listens_for_the_receive_window_after_each_measurement() {
  TestSite site;
  const Node downstream = downstreamNode(site);
  const uint64_t windowMs = downstream.config.upstream.receiveWindowMs;
  const uint64_t stormIntervalMs = downstream.config.mode.stormMeasureIntervalMs;
  downstream.levelSensor.setWaterLevelCm(levelBelowWatch(downstream));
  site.runMs(stormIntervalMs);
  downstream.rainGauge.tip();

  const uint64_t intervals = 10;
  uint64_t lastMeasuredAtMs = 0;
  bool measured = false;
  size_t reads = downstream.levelSensor.readCount();
  for (uint64_t step = 0; step < intervals * stormIntervalMs / kStepMs; ++step) {
    site.runMs(kStepMs);
    if (downstream.levelSensor.readCount() > reads) {
      reads = downstream.levelSensor.readCount();
      lastMeasuredAtMs = site.clock.nowMs();
      measured = true;
    }
    const bool windowOpen = measured && site.clock.nowMs() - lastMeasuredAtMs < windowMs;
    TEST_ASSERT_EQUAL(windowOpen, downstream.radio.isListening());
  }
  TEST_ASSERT_TRUE(measured);
}

static void test_the_downstream_node_sleeps_from_the_end_of_its_receive_window_to_its_next_measurement() {
  TestSite site;
  const Node downstream = downstreamNode(site);
  downstream.levelSensor.setWaterLevelCm(levelBelowWatch(downstream));
  downstream.rainGauge.tip();
  site.runMs(kStepMs);
  const uint64_t measuredAtMs = site.clock.nowMs();
  TEST_ASSERT_EQUAL_UINT(1, downstream.levelSensor.readCount());

  site.runMs(downstream.config.upstream.receiveWindowMs);

  TEST_ASSERT_FALSE(downstream.radio.isListening());
  TEST_ASSERT_TRUE(downstream.sleep.isAsleep());
  TEST_ASSERT_EQUAL_UINT64(measuredAtMs + downstream.config.mode.stormMeasureIntervalMs,
                           downstream.sleep.wakeAtMs());
}

static void test_a_node_with_no_upstream_node_keeps_its_receiver_off_through_a_flood() {
  TestSite site;
  const Node upstream = upstreamNode(site);
  upstream.rainGauge.tip();

  const AlertLevel levels[] = {AlertLevel::Watch, AlertLevel::Warning, AlertLevel::Evacuate};
  for (const AlertLevel level : levels) {
    upstream.levelSensor.setWaterLevelCm(levelWithin(upstream, level));
    TEST_ASSERT_EQUAL_UINT64(
        0, stepsListening(site, upstream, 5ull * upstream.config.mode.stormMeasureIntervalMs / kStepMs));
    ASSERT_ALERT_LEVEL(level, alertLevelOf(site, upstream));
  }
}

static void test_at_warning_the_downstream_node_listens_until_the_upstream_node_confirms() {
  TestSite site;
  const Node upstream = upstreamNode(site);
  const Node downstream = downstreamNode(site);
  const uint64_t steps = 5ull * downstream.config.mode.stormMeasureIntervalMs / kStepMs;
  holdLevel(site, downstream, levelWithin(downstream, AlertLevel::Warning));

  // Warning here, nothing heard from upstream yet: the receiver stays on between measurements.
  TEST_ASSERT_EQUAL_UINT64(steps, stepsListening(site, downstream, steps));

  holdLevel(site, upstream, levelWithin(upstream, AlertLevel::Warning));
  waitForAlertLevel(site, downstream, AlertLevel::Evacuate, kStepMs);

  // At Evacuate there is nothing left to listen for.
  TEST_ASSERT_EQUAL_UINT64(0, stepsListening(site, downstream, steps));
}

// --- Duty cycle ---

static void test_airtime_of_each_wadi_node_in_storm_mode_stays_under_the_duty_cycle_limit() {
  TestSite site;
  const Node nodes[] = {upstreamNode(site), downstreamNode(site)};
  const wadiwatch::RadioConfig& radio = site.config.radio;
  const uint64_t periodMs = radio.dutyCyclePeriodMs;
  const double allowedUs = static_cast<double>(periodMs) * 1000.0 * radio.dutyCycleLimitPercent / 100.0;

  // The busiest storm there is: it rains on both nodes and the flood takes each of
  // them through Watch and Warning to Evacuate, where they stay for hours.
  for (const Node& node : nodes) node.rainGauge.tip();
  const AlertLevel levels[] = {AlertLevel::Watch, AlertLevel::Warning, AlertLevel::Evacuate};
  for (const AlertLevel level : levels) {
    for (const Node& node : nodes) holdLevel(site, node, levelWithin(node, level));
  }

  // The first period holds the escalation; the later ones are steady re-sending.
  const uint64_t periods = 3;
  size_t firstFrame[] = {0, 0};
  for (uint64_t period = 1; period <= periods; ++period) {
    site.runMs(period * periodMs - site.clock.nowMs());
    for (size_t i = 0; i < 2; ++i) {
      const double usedUs = static_cast<double>(timeOnAirUs(site, nodes[i].radio, firstFrame[i]));
      TEST_ASSERT_TRUE_MESSAGE(usedUs < allowedUs, "a wadi node exceeded the duty-cycle limit");
      // The node really was sending all along: its alert once per re-send interval.
      const size_t frames = nodes[i].radio.sent().size() - firstFrame[i];
      if (period > 1) {
        TEST_ASSERT_TRUE(frames >= periodMs / nodes[i].config.alertResendIntervalMs);
      }
      firstFrame[i] = nodes[i].radio.sent().size();
    }
  }
  for (const Node& node : nodes) ASSERT_ALERT_LEVEL(AlertLevel::Evacuate, alertLevelOf(site, node));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_warning_at_the_upstream_node_and_at_the_downstream_node_produce_evacuate_downstream);
  RUN_TEST(test_evacuate_follows_within_one_resend_interval_whatever_the_offset_between_the_nodes);
  RUN_TEST(test_an_upstream_warning_heard_in_a_receive_window_is_remembered_until_the_node_reaches_warning);
  RUN_TEST(test_warning_at_the_upstream_node_alone_raises_no_alert_at_a_downstream_node_in_dry_mode);
  RUN_TEST(test_warning_at_the_upstream_node_alone_raises_no_alert_at_a_downstream_node_that_hears_it);
  RUN_TEST(test_warning_at_the_upstream_node_does_not_raise_a_downstream_watch);
  RUN_TEST(test_watch_at_the_upstream_node_does_not_confirm_a_downstream_warning);
  RUN_TEST(test_a_warning_still_in_force_downstream_is_confirmed_after_its_level_has_fallen);
  RUN_TEST(test_a_frame_that_arrived_before_the_receiver_was_switched_on_is_not_acted_on);
  RUN_TEST(test_warning_heard_from_a_node_that_is_not_the_upstream_node_has_no_effect);
  RUN_TEST(test_a_forged_upstream_warning_does_not_raise_evacuate);
  RUN_TEST(test_warning_at_the_downstream_node_does_not_raise_evacuate_at_the_upstream_node);
  RUN_TEST(test_which_node_is_upstream_of_which_comes_from_the_site_configuration);
  RUN_TEST(test_without_an_upstream_relationship_warning_at_both_nodes_stays_warning);
  RUN_TEST(test_a_node_at_warning_sends_its_alert_again_every_resend_interval);
  RUN_TEST(test_a_node_at_evacuate_sends_its_alert_again_every_resend_interval);
  RUN_TEST(test_a_node_at_watch_does_not_send_its_alert_again);
  RUN_TEST(test_an_alert_sent_again_carries_the_latest_measured_level);
  RUN_TEST(test_escalating_sends_the_higher_alert_at_once_and_then_every_resend_interval);
  RUN_TEST(test_an_alert_raised_between_two_measurements_is_sent_again_one_resend_interval_later);
  RUN_TEST(test_a_node_at_warning_stays_in_storm_mode_after_the_level_has_fallen);
  RUN_TEST(test_alerts_sent_again_do_not_restart_the_siren_pattern);
  RUN_TEST(test_in_dry_mode_neither_wadi_node_switches_its_receiver_on);
  RUN_TEST(test_in_storm_mode_the_downstream_node_listens_for_the_receive_window_after_each_measurement);
  RUN_TEST(test_the_downstream_node_sleeps_from_the_end_of_its_receive_window_to_its_next_measurement);
  RUN_TEST(test_a_node_with_no_upstream_node_keeps_its_receiver_off_through_a_flood);
  RUN_TEST(test_at_warning_the_downstream_node_listens_until_the_upstream_node_confirms);
  RUN_TEST(test_airtime_of_each_wadi_node_in_storm_mode_stays_under_the_duty_cycle_limit);
  return UNITY_END();
}
