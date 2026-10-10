// Scenario tests for the wadi node's dry mode, storm mode and heartbeat.
//
// Every duration comes from the site configuration. The scenarios watch the node
// minute by minute, so they assume the configured intervals are whole minutes.
#include <cmath>
#include <cstdint>
#include <vector>

#include <unity.h>

#include "wadiwatch/alert_engine.h"
#include "wadiwatch/packet.h"
#include "wadiwatch_testing/test_site.h"

using wadiwatch::AlertLevel;
using wadiwatch::levelThresholdCm;
using wadiwatch::Packet;
using wadiwatch::PacketKind;
using wadiwatch::WadiNodeConfig;
using wadiwatch::testing::Frame;
using wadiwatch::testing::TestSite;

void setUp() {}
void tearDown() {}

namespace {

const uint64_t kMinuteMs = 60 * 1000;

std::vector<Packet> packetsOnAir(const TestSite& site, PacketKind kind) {
  std::vector<Packet> packets;
  for (const Frame& frame : site.air.transmitted()) {
    Packet packet;
    if (wadiwatch::decodePacket(frame.data(), frame.size(), packet) && packet.kind == kind) {
      packets.push_back(packet);
    }
  }
  return packets;
}

size_t alertsOnAir(const TestSite& site, AlertLevel level) {
  size_t count = 0;
  for (const Packet& packet : packetsOnAir(site, PacketKind::Alert)) {
    if (packet.alertLevel == level) ++count;
  }
  return count;
}

size_t heartbeatsOnAir(const TestSite& site) {
  return packetsOnAir(site, PacketKind::Heartbeat).size();
}

// What someone standing beside the wadi node sees it do, minute by minute:
// when it pings its level sensor and when it sends a heartbeat.
class Observer {
 public:
  explicit Observer(TestSite& site) : site_(site) {}

  // Returns the number of minutes, out of those watched, in which the node measured.
  uint64_t watchMinutes(uint64_t minutes) {
    uint64_t minutesWithMeasurement = 0;
    for (uint64_t i = 0; i < minutes; ++i, ++minute_) {
      const size_t readsBefore = site_.levelSensor.readCount();
      const size_t heartbeatsBefore = heartbeatsOnAir(site_);
      site_.runMinutes(1);
      if (site_.levelSensor.readCount() > readsBefore) {
        measurementMinutes.push_back(minute_);
        ++minutesWithMeasurement;
      }
      if (heartbeatsOnAir(site_) > heartbeatsBefore) heartbeatMinutes.push_back(minute_);
    }
    return minutesWithMeasurement;
  }

  uint64_t minutesWatched() const { return minute_; }

  // Minutes since the start of the scenario.
  std::vector<uint64_t> measurementMinutes;
  std::vector<uint64_t> heartbeatMinutes;

 private:
  TestSite& site_;
  uint64_t minute_ = 0;
};

// The intervals of the site under test, in minutes.
struct Minutes {
  explicit Minutes(const WadiNodeConfig& config)
      : dryInterval(config.mode.dryMeasureIntervalMs / kMinuteMs),
        stormInterval(config.mode.stormMeasureIntervalMs / kMinuteMs),
        stormQuiet(config.mode.stormQuietMs / kMinuteMs),
        rainWindow(config.alertThresholds.watchRainWindowMs / kMinuteMs),
        heartbeatInterval(config.heartbeatIntervalMs / kMinuteMs) {}

  uint64_t dryInterval;
  uint64_t stormInterval;
  uint64_t stormQuiet;
  uint64_t rainWindow;
  uint64_t heartbeatInterval;
};

void assertEveryGapIs(uint64_t expectedGap, const std::vector<uint64_t>& minutes) {
  TEST_ASSERT_TRUE(minutes.size() >= 2);
  for (size_t i = 1; i < minutes.size(); ++i) {
    TEST_ASSERT_EQUAL_UINT64(expectedGap, minutes[i] - minutes[i - 1]);
  }
}

float levelBelowWatch(const WadiNodeConfig& config) {
  return levelThresholdCm(config.alertThresholds, AlertLevel::Watch) * 0.5f;
}

float levelBetweenWatchAndWarning(const WadiNodeConfig& config) {
  return (levelThresholdCm(config.alertThresholds, AlertLevel::Watch) +
          levelThresholdCm(config.alertThresholds, AlertLevel::Warning)) /
         2;
}

uint32_t tipsThatRaiseWatch(const WadiNodeConfig& config) {
  return static_cast<uint32_t>(
      std::ceil(config.alertThresholds.watchRainMm / config.rain.mmPerTip));
}

// Rain falls steadily, minute by minute, until `totalTips` have fallen.
void rainSteadily(TestSite& site, uint32_t totalTips, uint64_t minutes) {
  uint32_t fallen = 0;
  for (uint64_t minute = 0; minute < minutes; ++minute) {
    const uint32_t due = static_cast<uint32_t>(totalTips * (minute + 1) / minutes);
    site.rainGauge.tip(due - fallen);
    fallen = due;
    site.runMinutes(1);
  }
}

// The node takes its first measurement as soon as it starts.
void letTheNodeStart(TestSite& site) { site.runMinutes(1); }

}  // namespace

// --- Dry mode ---

static void test_on_a_dry_day_the_node_measures_every_dry_interval_and_sends_a_heartbeat_every_hour() {
  TestSite site;
  const Minutes minutes(site.config.wadiNode);
  const uint64_t day = 24 * 60;
  Observer observer(site);

  observer.watchMinutes(day);

  TEST_ASSERT_FALSE(observer.measurementMinutes.empty());
  TEST_ASSERT_EQUAL_UINT64(0, observer.measurementMinutes.front());
  assertEveryGapIs(minutes.dryInterval, observer.measurementMinutes);
  TEST_ASSERT_EQUAL_UINT64(day / minutes.dryInterval, observer.measurementMinutes.size());

  // The first heartbeat comes one interval after the node starts, then one per interval.
  TEST_ASSERT_FALSE(observer.heartbeatMinutes.empty());
  TEST_ASSERT_EQUAL_UINT64(minutes.heartbeatInterval, observer.heartbeatMinutes.front());
  assertEveryGapIs(minutes.heartbeatInterval, observer.heartbeatMinutes);
  TEST_ASSERT_TRUE(observer.heartbeatMinutes.back() + minutes.heartbeatInterval >= day);

  // Nothing else goes on the air, and the node wakes only to measure.
  TEST_ASSERT_EQUAL_UINT(observer.heartbeatMinutes.size(), site.air.transmitted().size());
  TEST_ASSERT_EQUAL_UINT(observer.measurementMinutes.size(), site.wadiNodeSleep.requestCount());
  TEST_ASSERT_FALSE(site.siren.isOn());
}

static void test_in_dry_mode_the_node_asks_to_sleep_until_its_next_measurement() {
  TestSite site;

  letTheNodeStart(site);

  TEST_ASSERT_TRUE(site.wadiNodeSleep.isAsleep());
  TEST_ASSERT_EQUAL_UINT64(site.config.wadiNode.mode.dryMeasureIntervalMs,
                           site.wadiNodeSleep.lastRequestMs());
}

static void test_a_node_woken_early_without_rain_goes_back_to_sleep_without_measuring() {
  TestSite site;
  letTheNodeStart(site);
  const uint64_t wakeAtMs = site.wadiNodeSleep.wakeAtMs();
  const size_t reads = site.levelSensor.readCount();
  const size_t sleepRequests = site.wadiNodeSleep.requestCount();

  site.clock.advanceMs(site.config.wadiNode.mode.dryMeasureIntervalMs / 3);
  site.wadiNode.tick();

  TEST_ASSERT_EQUAL_UINT(reads, site.levelSensor.readCount());
  TEST_ASSERT_EQUAL_UINT(sleepRequests + 1, site.wadiNodeSleep.requestCount());
  TEST_ASSERT_EQUAL_UINT64(wakeAtMs, site.wadiNodeSleep.wakeAtMs());
  TEST_ASSERT_EQUAL_UINT(0, site.air.transmitted().size());
}

// --- Entering storm mode ---

static void test_a_rain_gauge_tip_wakes_the_node_into_storm_mode() {
  TestSite site;
  const Minutes minutes(site.config.wadiNode);
  Observer observer(site);
  observer.watchMinutes(minutes.dryInterval + minutes.dryInterval / 2);
  TEST_ASSERT_TRUE(site.wadiNodeSleep.isAsleep());
  const uint64_t minuteOfTheTip = observer.minutesWatched();
  const size_t dryMeasurements = observer.measurementMinutes.size();

  site.rainGauge.tip();
  const uint64_t measured = observer.watchMinutes(minutes.stormQuiet);

  // It measures at once, then once per storm interval.
  TEST_ASSERT_TRUE(observer.measurementMinutes.size() > dryMeasurements);
  TEST_ASSERT_EQUAL_UINT64(minuteOfTheTip, observer.measurementMinutes[dryMeasurements]);
  TEST_ASSERT_EQUAL_UINT64(minutes.stormQuiet / minutes.stormInterval, measured);
  // A little rain is not an alert.
  TEST_ASSERT_EQUAL_UINT(0, packetsOnAir(site, PacketKind::Alert).size());
}

static void test_in_storm_mode_the_node_asks_to_sleep_until_its_next_measurement() {
  TestSite site;
  site.rainGauge.tip();

  letTheNodeStart(site);

  TEST_ASSERT_TRUE(site.wadiNodeSleep.isAsleep());
  TEST_ASSERT_EQUAL_UINT64(site.config.wadiNode.mode.stormMeasureIntervalMs,
                           site.wadiNodeSleep.lastRequestMs());
}

static void test_a_level_above_the_watch_threshold_enters_storm_mode_without_rain() {
  TestSite site;
  const Minutes minutes(site.config.wadiNode);
  Observer observer(site);
  observer.watchMinutes(minutes.dryInterval + minutes.dryInterval / 2);

  site.levelSensor.setWaterLevelCm(levelBetweenWatchAndWarning(site.config.wadiNode));
  // The next dry-mode measurement finds the higher level.
  observer.watchMinutes(minutes.dryInterval);
  const uint64_t measured = observer.watchMinutes(minutes.dryInterval);

  TEST_ASSERT_EQUAL_UINT64(minutes.dryInterval / minutes.stormInterval, measured);
  TEST_ASSERT_EQUAL_UINT(1, alertsOnAir(site, AlertLevel::Watch));
  TEST_ASSERT_FALSE(site.siren.isOn());
}

// --- Rain raises Watch ---

static void test_heavy_rain_within_the_window_raises_watch_with_the_level_still_low() {
  TestSite site;
  const WadiNodeConfig& config = site.config.wadiNode;
  const Minutes minutes(config);
  site.levelSensor.setWaterLevelCm(levelBelowWatch(config));

  // One minute short of the window, the rain is one tip short of the threshold.
  rainSteadily(site, tipsThatRaiseWatch(config) - 1, minutes.rainWindow - 1);
  TEST_ASSERT_EQUAL_UINT(0, packetsOnAir(site, PacketKind::Alert).size());

  site.rainGauge.tip();
  site.runMinutes(1);

  TEST_ASSERT_EQUAL_UINT(1, alertsOnAir(site, AlertLevel::Watch));
  TEST_ASSERT_FALSE(site.siren.isOn());

  // More rain does not repeat the alert.
  site.rainGauge.rainMm(config.alertThresholds.watchRainMm);
  site.runMinutes(minutes.stormInterval);
  TEST_ASSERT_EQUAL_UINT(1, packetsOnAir(site, PacketKind::Alert).size());
}

static void test_the_same_rain_spread_over_twice_the_window_does_not_raise_watch() {
  TestSite site;
  const WadiNodeConfig& config = site.config.wadiNode;
  const Minutes minutes(config);
  site.levelSensor.setWaterLevelCm(levelBelowWatch(config));

  rainSteadily(site, tipsThatRaiseWatch(config), 2 * minutes.rainWindow);

  TEST_ASSERT_EQUAL_UINT(0, packetsOnAir(site, PacketKind::Alert).size());
}

// --- Returning to dry mode ---

static void test_the_node_returns_to_dry_mode_only_after_the_quiet_period_below_the_watch_threshold() {
  TestSite site;
  const WadiNodeConfig& config = site.config.wadiNode;
  const Minutes minutes(config);
  Observer observer(site);
  site.levelSensor.setWaterLevelCm(levelBetweenWatchAndWarning(config));
  observer.watchMinutes(minutes.dryInterval);

  site.levelSensor.setWaterLevelCm(levelBelowWatch(config));

  // Storm mode lasts for the whole quiet period.
  TEST_ASSERT_EQUAL_UINT64(minutes.stormQuiet / minutes.stormInterval,
                           observer.watchMinutes(minutes.stormQuiet));
  // Then the node switches over and measures once per dry interval.
  observer.watchMinutes(minutes.stormInterval);
  const uint64_t dryIntervalsWatched = 4;
  TEST_ASSERT_EQUAL_UINT64(dryIntervalsWatched,
                           observer.watchMinutes(dryIntervalsWatched * minutes.dryInterval));
}

static void test_dips_below_the_watch_threshold_shorter_than_the_quiet_period_do_not_end_storm_mode() {
  TestSite site;
  const WadiNodeConfig& config = site.config.wadiNode;
  const Minutes minutes(config);
  Observer observer(site);
  site.levelSensor.setWaterLevelCm(levelBetweenWatchAndWarning(config));
  observer.watchMinutes(minutes.dryInterval);
  const uint64_t minutesBefore = observer.minutesWatched();

  // The level hovers around the threshold: each dip is one measurement short of the quiet period.
  uint64_t measured = 0;
  for (int dip = 0; dip < 3; ++dip) {
    site.levelSensor.setWaterLevelCm(levelBelowWatch(config));
    measured += observer.watchMinutes(minutes.stormQuiet - minutes.stormInterval);
    site.levelSensor.setWaterLevelCm(levelBetweenWatchAndWarning(config));
    measured += observer.watchMinutes(minutes.stormInterval);
  }

  TEST_ASSERT_EQUAL_UINT64((observer.minutesWatched() - minutesBefore) / minutes.stormInterval,
                           measured);
  // Hovering does not repeat the Watch alert either.
  TEST_ASSERT_EQUAL_UINT(1, packetsOnAir(site, PacketKind::Alert).size());
}

static void test_rain_during_the_quiet_period_starts_the_quiet_period_again() {
  TestSite site;
  const Minutes minutes(site.config.wadiNode);
  Observer observer(site);
  site.rainGauge.tip();
  uint64_t measured = observer.watchMinutes(minutes.stormQuiet - minutes.stormInterval);

  site.rainGauge.tip();
  measured += observer.watchMinutes(minutes.stormQuiet);

  TEST_ASSERT_EQUAL_UINT64(observer.minutesWatched() / minutes.stormInterval, measured);

  // With no more rain the node goes back to dry mode.
  observer.watchMinutes(minutes.stormInterval);
  const uint64_t dryIntervalsWatched = 4;
  TEST_ASSERT_EQUAL_UINT64(dryIntervalsWatched,
                           observer.watchMinutes(dryIntervalsWatched * minutes.dryInterval));
}

// --- Heartbeat ---

static void test_a_heartbeat_carries_battery_voltage_signal_strength_last_reading_and_tamper_flag() {
  TestSite site;
  const WadiNodeConfig& config = site.config.wadiNode;
  const uint16_t batteryMillivolts = 3920;
  const int8_t signalStrengthDbm = -104;
  const float levelCm = levelBelowWatch(config);
  site.wadiNodeHealth.setBatteryMillivolts(batteryMillivolts);
  site.wadiNodeHealth.setSignalStrengthDbm(signalStrengthDbm);
  site.wadiNodeHealth.setTamper(true);
  site.levelSensor.setWaterLevelCm(levelCm);

  site.runMs(config.heartbeatIntervalMs + kMinuteMs);

  const std::vector<Packet> heartbeats = packetsOnAir(site, PacketKind::Heartbeat);
  TEST_ASSERT_EQUAL_UINT(1, heartbeats.size());
  TEST_ASSERT_EQUAL_UINT16(config.id, heartbeats[0].sender);
  TEST_ASSERT_EQUAL_UINT16(batteryMillivolts, heartbeats[0].heartbeat.batteryMillivolts);
  TEST_ASSERT_EQUAL_INT8(signalStrengthDbm, heartbeats[0].heartbeat.signalStrengthDbm);
  TEST_ASSERT_TRUE(heartbeats[0].heartbeat.tamper);
  TEST_ASSERT_TRUE(heartbeats[0].heartbeat.hasLastLevel);
  TEST_ASSERT_INT16_WITHIN(1, static_cast<int16_t>(levelCm), heartbeats[0].heartbeat.lastLevelCm);
}

static void test_each_heartbeat_reports_the_values_of_its_own_hour() {
  TestSite site;
  const WadiNodeConfig& config = site.config.wadiNode;
  site.wadiNodeHealth.setBatteryMillivolts(3920);
  site.wadiNodeHealth.setTamper(true);
  site.levelSensor.setWaterLevelCm(levelBelowWatch(config));
  site.runMs(config.heartbeatIntervalMs + kMinuteMs);

  const uint16_t lowerBatteryMillivolts = 3610;
  const float lowerLevelCm = levelBelowWatch(config) / 2;
  site.wadiNodeHealth.setBatteryMillivolts(lowerBatteryMillivolts);
  site.wadiNodeHealth.setTamper(false);
  site.levelSensor.setWaterLevelCm(lowerLevelCm);
  site.runMs(config.heartbeatIntervalMs);

  const std::vector<Packet> heartbeats = packetsOnAir(site, PacketKind::Heartbeat);
  TEST_ASSERT_EQUAL_UINT(2, heartbeats.size());
  TEST_ASSERT_EQUAL_UINT16(lowerBatteryMillivolts, heartbeats[1].heartbeat.batteryMillivolts);
  TEST_ASSERT_FALSE(heartbeats[1].heartbeat.tamper);
  TEST_ASSERT_INT16_WITHIN(1, static_cast<int16_t>(lowerLevelCm), heartbeats[1].heartbeat.lastLevelCm);
}

static void test_in_storm_mode_the_node_still_sends_a_heartbeat_every_hour() {
  TestSite site;
  const Minutes minutes(site.config.wadiNode);
  Observer observer(site);
  site.levelSensor.setWaterLevelCm(levelBetweenWatchAndWarning(site.config.wadiNode));
  const uint64_t hours = 3;

  const uint64_t measured = observer.watchMinutes(hours * minutes.heartbeatInterval + 1);

  TEST_ASSERT_EQUAL_UINT64(observer.minutesWatched() / minutes.stormInterval, measured);
  TEST_ASSERT_EQUAL_UINT64(hours, observer.heartbeatMinutes.size());
  TEST_ASSERT_EQUAL_UINT64(minutes.heartbeatInterval, observer.heartbeatMinutes.front());
  assertEveryGapIs(minutes.heartbeatInterval, observer.heartbeatMinutes);
}

// --- Heartbeat packet format ---

static void test_heartbeat_packet_survives_encoding_and_decoding() {
  Packet sent = Packet();
  sent.kind = PacketKind::Heartbeat;
  sent.sender = 0x0203;
  sent.heartbeat.batteryMillivolts = 4150;
  sent.heartbeat.signalStrengthDbm = -119;
  sent.heartbeat.tamper = true;
  sent.heartbeat.hasLastLevel = true;
  sent.heartbeat.lastLevelCm = -3;
  uint8_t frame[wadiwatch::kMaxPacketSize];

  const size_t length = wadiwatch::encodePacket(sent, frame, sizeof(frame));
  Packet received = Packet();
  TEST_ASSERT_TRUE(wadiwatch::decodePacket(frame, length, received));

  TEST_ASSERT_TRUE(received.kind == PacketKind::Heartbeat);
  TEST_ASSERT_EQUAL_UINT16(sent.sender, received.sender);
  TEST_ASSERT_EQUAL_UINT16(sent.heartbeat.batteryMillivolts, received.heartbeat.batteryMillivolts);
  TEST_ASSERT_EQUAL_INT8(sent.heartbeat.signalStrengthDbm, received.heartbeat.signalStrengthDbm);
  TEST_ASSERT_TRUE(received.heartbeat.tamper);
  TEST_ASSERT_TRUE(received.heartbeat.hasLastLevel);
  TEST_ASSERT_EQUAL_INT16(sent.heartbeat.lastLevelCm, received.heartbeat.lastLevelCm);
}

static void test_heartbeat_packet_can_say_that_no_level_has_been_measured() {
  Packet sent = Packet();
  sent.kind = PacketKind::Heartbeat;
  uint8_t frame[wadiwatch::kMaxPacketSize];

  const size_t length = wadiwatch::encodePacket(sent, frame, sizeof(frame));
  Packet received = Packet();
  TEST_ASSERT_TRUE(wadiwatch::decodePacket(frame, length, received));

  TEST_ASSERT_FALSE(received.heartbeat.hasLastLevel);
  TEST_ASSERT_FALSE(received.heartbeat.tamper);
}

static void test_truncated_heartbeat_packet_is_rejected() {
  Packet sent = Packet();
  sent.kind = PacketKind::Heartbeat;
  uint8_t frame[wadiwatch::kMaxPacketSize];
  const size_t length = wadiwatch::encodePacket(sent, frame, sizeof(frame));

  Packet received = Packet();
  TEST_ASSERT_FALSE(wadiwatch::decodePacket(frame, length - 1, received));
  TEST_ASSERT_EQUAL_UINT(0, wadiwatch::encodePacket(sent, frame, length - 1));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_on_a_dry_day_the_node_measures_every_dry_interval_and_sends_a_heartbeat_every_hour);
  RUN_TEST(test_in_dry_mode_the_node_asks_to_sleep_until_its_next_measurement);
  RUN_TEST(test_a_node_woken_early_without_rain_goes_back_to_sleep_without_measuring);
  RUN_TEST(test_a_rain_gauge_tip_wakes_the_node_into_storm_mode);
  RUN_TEST(test_in_storm_mode_the_node_asks_to_sleep_until_its_next_measurement);
  RUN_TEST(test_a_level_above_the_watch_threshold_enters_storm_mode_without_rain);
  RUN_TEST(test_heavy_rain_within_the_window_raises_watch_with_the_level_still_low);
  RUN_TEST(test_the_same_rain_spread_over_twice_the_window_does_not_raise_watch);
  RUN_TEST(test_the_node_returns_to_dry_mode_only_after_the_quiet_period_below_the_watch_threshold);
  RUN_TEST(test_dips_below_the_watch_threshold_shorter_than_the_quiet_period_do_not_end_storm_mode);
  RUN_TEST(test_rain_during_the_quiet_period_starts_the_quiet_period_again);
  RUN_TEST(test_a_heartbeat_carries_battery_voltage_signal_strength_last_reading_and_tamper_flag);
  RUN_TEST(test_each_heartbeat_reports_the_values_of_its_own_hour);
  RUN_TEST(test_in_storm_mode_the_node_still_sends_a_heartbeat_every_hour);
  RUN_TEST(test_heartbeat_packet_survives_encoding_and_decoding);
  RUN_TEST(test_heartbeat_packet_can_say_that_no_level_has_been_measured);
  RUN_TEST(test_truncated_heartbeat_packet_is_rejected);
  return UNITY_END();
}
