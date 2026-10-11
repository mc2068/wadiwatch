// Scenario tests for robust measurement and the rate of rise: what a noisy level
// sensor and a fast-rising wadi make the wadi node put on the air, and what a
// bystander at the siren node hears.
//
// Every threshold and duration comes from the site configuration. The scenarios
// follow the node from one storm-mode measurement to the next.
#include <cmath>
#include <cstdint>
#include <vector>

#include <unity.h>

#include "wadiwatch/alert_engine.h"
#include "wadiwatch/level_measurement.h"
#include "wadiwatch/packet.h"
#include "wadiwatch_testing/test_site.h"

using wadiwatch::AlertLevel;
using wadiwatch::AlertThresholds;
using wadiwatch::kPingsPerMeasurement;
using wadiwatch::levelThresholdCm;
using wadiwatch::Packet;
using wadiwatch::PacketKind;
using wadiwatch::SiteConfig;
using wadiwatch::WadiNodeConfig;
using wadiwatch::testing::SirenSound;
using wadiwatch::testing::TestSite;

void setUp() {}
void tearDown() {}

#define ASSERT_ALERT_LEVEL(expected, actual) \
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected), static_cast<uint8_t>(actual))

#define ASSERT_SIREN_SOUND(expected, actual) \
  TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual))

namespace {

const uint64_t kMinuteMs = 60 * 1000;

// Weather for the temperature scenarios: a summer afternoon and a winter night.
const float kHotAirC = 40.0f;
const float kFreezingAirC = 0.0f;

float thresholdCm(const TestSite& site, AlertLevel alertLevel) {
  return levelThresholdCm(site.config.wadiNode.alertThresholds, alertLevel);
}

// A level well below the Watch threshold: by itself it calls for nothing.
float lowLevelCm(const TestSite& site) { return thresholdCm(site, AlertLevel::Watch) / 2.0f; }

// A level as packets carry it: whole millimetres. The same for a rate of rise per minute.
int toMm(float cm) { return static_cast<int>(std::lround(cm * 10.0f)); }

// Rainfall as packets carry it: tenths of a millimetre.
int toTenthsMm(float mm) { return static_cast<int>(std::lround(mm * 10.0f)); }

std::vector<Packet> readingsOnAir(const TestSite& site) {
  return site.packetsOnAir(PacketKind::Reading);
}

// Everything the wadi node has sent. The siren node transmits too: it
// acknowledges alerts and sends heartbeats of its own.
std::vector<Packet> wadiNodePackets(const TestSite& site) {
  return site.packetsOnAirFrom(site.config.wadiNode.id);
}

std::vector<Packet> wadiNodeHeartbeats(const TestSite& site) {
  std::vector<Packet> heartbeats;
  for (const Packet& packet : wadiNodePackets(site)) {
    if (packet.kind == PacketKind::Heartbeat) heartbeats.push_back(packet);
  }
  return heartbeats;
}

// Runs until the wadi node has taken its next measurement, whichever operating mode it is in.
void runUntilMeasured(TestSite& site) {
  const size_t pingsBefore = site.levelSensor.readCount();
  for (uint64_t waited = 0; site.levelSensor.readCount() == pingsBefore;
       waited += TestSite::kStepMs) {
    TEST_ASSERT_TRUE_MESSAGE(waited < site.config.wadiNode.mode.dryMeasureIntervalMs,
                             "the wadi node never measured the level");
    site.runMs(TestSite::kStepMs);
  }
}

// Holds the wadi at one level until the wadi node has measured it.
void measureAt(TestSite& site, float levelCm) {
  site.levelSensor.setWaterLevelCm(levelCm);
  runUntilMeasured(site);
}

// It starts to rain: the first tip of the rain gauge puts the node in storm mode,
// where it measures, and reports, once per storm-mode interval.
void beginStorm(TestSite& site, float levelCm) {
  site.rainGauge.tip();
  measureAt(site, levelCm);
}

// How much the wadi rises from one storm-mode measurement to the next at this rate.
float risePerMeasurementCm(const TestSite& site, float cmPerMin) {
  return cmPerMin * static_cast<float>(site.config.wadiNode.mode.stormMeasureIntervalMs) /
         static_cast<float>(kMinuteMs);
}

// The wadi rises at a steady rate over this many storm-mode measurements.
// Returns the level it has reached.
float riseSteadily(TestSite& site, float fromCm, float cmPerMin, uint32_t measurements) {
  float levelCm = fromCm;
  for (uint32_t i = 0; i < measurements; ++i) {
    levelCm += risePerMeasurementCm(site, cmPerMin);
    measureAt(site, levelCm);
  }
  return levelCm;
}

// The wadi holds its level over this many storm-mode measurements.
void holdSteady(TestSite& site, uint32_t measurements) {
  for (uint32_t i = 0; i < measurements; ++i) runUntilMeasured(site);
}

// Storm-mode measurements it takes for a rise to count as sustained.
uint32_t sustainMeasurements(const TestSite& site) {
  return site.config.wadiNode.alertThresholds.warningRateOfRiseSustainMs /
         site.config.wadiNode.mode.stormMeasureIntervalMs;
}

// Storm-mode measurements that span the rate-of-rise window.
uint32_t windowMeasurements(const TestSite& site) {
  return site.config.wadiNode.rateOfRise.windowMs /
         site.config.wadiNode.mode.stormMeasureIntervalMs;
}

// A rate of rise clearly at or above the Warning threshold of the prototype site.
float fastRiseCmPerMin() {
  return wadiwatch::prototypeSite().wadiNode.alertThresholds.warningRateOfRiseCmPerMin * 1.2f;
}

// Where the fast-rise scenarios start: a trickle in the bed.
float trickleCm(const TestSite& site) { return thresholdCm(site, AlertLevel::Watch) / 10.0f; }

// A distance nearer than the sensor can measure: mud or a web on the sensor face.
// Read as a level it would be water over the banks.
float impossiblyNearCm(const TestSite& site) {
  return site.config.wadiNode.levelSensor.minDistanceCm / 2.0f;
}

// A distance farther than the wadi bed can be.
float impossiblyFarCm(const TestSite& site) {
  return site.config.wadiNode.mountingHeightCm +
         2.0f * site.config.wadiNode.levelSensor.bedToleranceCm;
}

// Something in the beam where the water would stand at Evacuate: floating debris, a branch.
float debrisLevelCm(const TestSite& site) {
  return (thresholdCm(site, AlertLevel::Evacuate) +
          site.config.wadiNode.alertThresholds.bankFullDepthCm) /
         2.0f;
}

// One ping of the next measurement, the one at `position`, is returned by something
// at `outlierLevelCm` instead of by the water.
void scriptOutlierPing(TestSite& site, float waterLevelCm, size_t position, float outlierLevelCm) {
  for (size_t i = 0; i < position; ++i) site.levelSensor.scriptPingLevelCm(waterLevelCm);
  site.levelSensor.scriptPingLevelCm(outlierLevelCm);
}

// What a level sensor without temperature correction makes of this level in this air.
float uncorrectedLevelCm(const TestSite& site, float levelCm, float airC) {
  const WadiNodeConfig& config = site.config.wadiNode;
  const float distanceCm = config.mountingHeightCm - levelCm;
  return config.mountingHeightCm -
         distanceCm * wadiwatch::speedOfSoundMps(config.levelSensor.referenceTemperatureC) /
             wadiwatch::speedOfSoundMps(airC);
}

}  // namespace

// --- The median of five pings ---

static void test_single_outlier_pings_do_not_change_the_measured_level_or_raise_an_alert() {
  TestSite site;
  const float levelCm = lowLevelCm(site);
  // Spray thrown up under the sensor, and a ping that slips past the water to the bed.
  const float outlierLevelsCm[] = {debrisLevelCm(site), 0.0f};
  site.rainGauge.tip();
  site.levelSensor.setWaterLevelCm(levelCm);

  // Every measurement has one outlier among its pings, each time a different ping.
  const size_t measurements = 2 * kPingsPerMeasurement;
  for (size_t i = 0; i < measurements; ++i) {
    scriptOutlierPing(site, levelCm, i % kPingsPerMeasurement, outlierLevelsCm[i % 2]);
    runUntilMeasured(site);
    TEST_ASSERT_EQUAL_UINT(0, site.levelSensor.scriptedPingsLeft());
  }

  const std::vector<Packet> readings = readingsOnAir(site);
  TEST_ASSERT_EQUAL_UINT(measurements, readings.size());
  for (const Packet& reading : readings) {
    TEST_ASSERT_INT_WITHIN(1, toMm(levelCm), reading.reading.levelMm);
    TEST_ASSERT_EQUAL_INT16(0, reading.reading.rateOfRiseMmPerMin);
    ASSERT_ALERT_LEVEL(AlertLevel::None, reading.reading.alertLevel);
  }
  TEST_ASSERT_EQUAL_UINT(0, site.packetsOnAir(PacketKind::Alert).size());
  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
  TEST_ASSERT_FALSE(site.siren.isStrobeOn());
}

static void test_outlier_pings_are_not_counted_as_discarded_readings() {
  TestSite site;
  const float levelCm = lowLevelCm(site);
  site.levelSensor.setWaterLevelCm(levelCm);
  scriptOutlierPing(site, levelCm, 0, debrisLevelCm(site));

  site.runMs(site.config.wadiNode.heartbeatIntervalMs + kMinuteMs);

  const std::vector<Packet> heartbeats = wadiNodeHeartbeats(site);
  TEST_ASSERT_EQUAL_UINT(1, heartbeats.size());
  TEST_ASSERT_EQUAL_UINT8(0, heartbeats[0].heartbeat.discardedReadings);
  TEST_ASSERT_INT_WITHIN(1, toMm(levelCm), heartbeats[0].heartbeat.lastLevelMm);
}

static void test_in_dry_mode_an_outlier_ping_does_not_wake_the_node_into_storm_mode() {
  TestSite site;
  const float levelCm = lowLevelCm(site);
  site.levelSensor.setWaterLevelCm(levelCm);
  scriptOutlierPing(site, levelCm, kPingsPerMeasurement / 2, debrisLevelCm(site));

  const uint64_t dryIntervals = 3;
  site.runMs(dryIntervals * site.config.wadiNode.mode.dryMeasureIntervalMs);

  // One measurement of five pings per dry-mode interval, and nothing on the air.
  TEST_ASSERT_EQUAL_UINT(dryIntervals * kPingsPerMeasurement, site.levelSensor.readCount());
  TEST_ASSERT_EQUAL_UINT(0, site.air.transmitted().size());
}

// --- Physically impossible readings ---

static void test_readings_outside_the_possible_range_are_discarded_and_counted_in_the_heartbeat() {
  TestSite site;
  const WadiNodeConfig& config = site.config.wadiNode;
  const float levelCm = lowLevelCm(site);
  site.levelSensor.setWaterLevelCm(levelCm);
  // The first measurement loses two of its pings and still has a majority left.
  site.levelSensor.scriptPingDistanceCm(impossiblyNearCm(site));
  site.levelSensor.scriptPingLevelCm(levelCm);
  site.levelSensor.scriptPingDistanceCm(impossiblyFarCm(site));
  site.levelSensor.scriptPingLevelCm(levelCm);
  site.levelSensor.scriptPingLevelCm(levelCm);
  // The second is impossible throughout. Believed, it would be water over the banks.
  TEST_ASSERT_TRUE(config.mountingHeightCm - impossiblyNearCm(site) >
                   thresholdCm(site, AlertLevel::Evacuate));
  for (size_t i = 0; i < kPingsPerMeasurement; ++i) {
    site.levelSensor.scriptPingDistanceCm(impossiblyNearCm(site));
  }
  const unsigned discarded = 2 + kPingsPerMeasurement;

  site.runMs(config.heartbeatIntervalMs + kMinuteMs);

  TEST_ASSERT_EQUAL_UINT(0, site.levelSensor.scriptedPingsLeft());
  const std::vector<Packet> heartbeats = wadiNodeHeartbeats(site);
  TEST_ASSERT_EQUAL_UINT(1, heartbeats.size());
  TEST_ASSERT_EQUAL_UINT8(discarded, heartbeats[0].heartbeat.discardedReadings);
  // The level is the one the possible pings gave.
  TEST_ASSERT_TRUE(heartbeats[0].heartbeat.hasLastLevel);
  TEST_ASSERT_INT_WITHIN(1, toMm(levelCm), heartbeats[0].heartbeat.lastLevelMm);
  // Counted, not alerted on: the heartbeat is all the node has sent, and it never
  // left dry mode.
  TEST_ASSERT_EQUAL_UINT(1, wadiNodePackets(site).size());
  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
}

static void test_a_ping_without_an_echo_is_counted_as_a_discarded_reading() {
  TestSite site;
  const float levelCm = lowLevelCm(site);
  site.levelSensor.setWaterLevelCm(levelCm);
  site.levelSensor.scriptPingWithoutEcho();

  site.runMs(site.config.wadiNode.heartbeatIntervalMs + kMinuteMs);

  const std::vector<Packet> heartbeats = wadiNodeHeartbeats(site);
  TEST_ASSERT_EQUAL_UINT(1, heartbeats.size());
  TEST_ASSERT_EQUAL_UINT8(1, heartbeats[0].heartbeat.discardedReadings);
  TEST_ASSERT_INT_WITHIN(1, toMm(levelCm), heartbeats[0].heartbeat.lastLevelMm);
}

static void test_each_heartbeat_counts_the_readings_discarded_since_the_one_before() {
  TestSite site;
  const WadiNodeConfig& config = site.config.wadiNode;
  site.levelSensor.setWaterLevelCm(lowLevelCm(site));
  site.levelSensor.scriptPingDistanceCm(impossiblyNearCm(site));
  site.runMs(config.heartbeatIntervalMs + kMinuteMs);

  // The sensor reads cleanly for the whole of the next hour, then misreads three times.
  site.runMs(config.heartbeatIntervalMs);
  for (int i = 0; i < 3; ++i) site.levelSensor.scriptPingDistanceCm(impossiblyFarCm(site));
  site.runMs(config.heartbeatIntervalMs);

  const std::vector<Packet> heartbeats = wadiNodeHeartbeats(site);
  TEST_ASSERT_EQUAL_UINT(3, heartbeats.size());
  TEST_ASSERT_EQUAL_UINT8(1, heartbeats[0].heartbeat.discardedReadings);
  TEST_ASSERT_EQUAL_UINT8(0, heartbeats[1].heartbeat.discardedReadings);
  TEST_ASSERT_EQUAL_UINT8(3, heartbeats[2].heartbeat.discardedReadings);
}

static void test_a_fouled_sensor_shows_up_in_the_heartbeat_and_never_as_a_flood() {
  TestSite site;
  const WadiNodeConfig& config = site.config.wadiNode;
  // Mud on the sensor face from the start: every ping comes back from right in front of it.
  const uint64_t measurementsInAnHour =
      config.heartbeatIntervalMs / config.mode.dryMeasureIntervalMs + 1;
  for (uint64_t i = 0; i < measurementsInAnHour * kPingsPerMeasurement; ++i) {
    site.levelSensor.scriptPingDistanceCm(impossiblyNearCm(site));
  }

  site.runMs(config.heartbeatIntervalMs + kMinuteMs);

  // The node stayed in dry mode and sent nothing but its heartbeat.
  TEST_ASSERT_EQUAL_UINT(measurementsInAnHour * kPingsPerMeasurement,
                         site.levelSensor.readCount());
  TEST_ASSERT_EQUAL_UINT(1, wadiNodePackets(site).size());
  const std::vector<Packet> heartbeats = wadiNodeHeartbeats(site);
  TEST_ASSERT_EQUAL_UINT(1, heartbeats.size());
  TEST_ASSERT_EQUAL_UINT8(measurementsInAnHour * kPingsPerMeasurement,
                          heartbeats[0].heartbeat.discardedReadings);
  TEST_ASSERT_FALSE(heartbeats[0].heartbeat.hasLastLevel);
  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
}

static void test_the_discard_count_in_a_heartbeat_stops_at_its_largest_value() {
  TestSite site;
  const uint8_t largestCount = 255;
  // The sensor dies as a storm begins, with the node measuring every storm-mode interval.
  site.levelSensor.setEchoing(false);
  site.rainGauge.tip();

  site.runMs(site.config.wadiNode.heartbeatIntervalMs + kMinuteMs);

  TEST_ASSERT_TRUE(site.levelSensor.readCount() > largestCount);
  const std::vector<Packet> heartbeats = wadiNodeHeartbeats(site);
  TEST_ASSERT_EQUAL_UINT(1, heartbeats.size());
  TEST_ASSERT_EQUAL_UINT8(largestCount, heartbeats[0].heartbeat.discardedReadings);
  TEST_ASSERT_EQUAL_UINT(0, site.packetsOnAir(PacketKind::Alert).size());
}

// --- Rate of rise ---

static void test_a_fast_sustained_rise_at_a_low_level_raises_warning_by_rate_of_rise_alone() {
  TestSite site;
  const float cmPerMin = fastRiseCmPerMin();
  float levelCm = trickleCm(site);
  beginStorm(site, levelCm);

  // One measurement short of the sustain time the rise is not yet an alert.
  levelCm = riseSteadily(site, levelCm, cmPerMin, sustainMeasurements(site) - 1);
  TEST_ASSERT_EQUAL_UINT(0, site.packetsOnAir(PacketKind::Alert).size());
  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());

  levelCm = riseSteadily(site, levelCm, cmPerMin, 1);

  // The level alone still calls for nothing: it is below even the Watch threshold.
  TEST_ASSERT_TRUE(levelCm < thresholdCm(site, AlertLevel::Watch));
  const std::vector<Packet> alerts = site.packetsOnAir(PacketKind::Alert);
  TEST_ASSERT_EQUAL_UINT(1, alerts.size());
  ASSERT_ALERT_LEVEL(AlertLevel::Warning, alerts[0].alert.alertLevel);
  TEST_ASSERT_INT_WITHIN(1, toMm(levelCm), alerts[0].alert.levelMm);
  TEST_ASSERT_INT_WITHIN(1, toMm(cmPerMin), alerts[0].alert.rateOfRiseMmPerMin);
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());
  TEST_ASSERT_TRUE(site.siren.isStrobeOn());
}

static void test_a_rise_at_exactly_the_threshold_rate_raises_warning() {
  TestSite site;
  const float cmPerMin = site.config.wadiNode.alertThresholds.warningRateOfRiseCmPerMin;
  // Whole centimetres all the way, so that the sensor's arithmetic is exact.
  beginStorm(site, 10.0f);

  riseSteadily(site, 10.0f, cmPerMin, sustainMeasurements(site));

  ASSERT_ALERT_LEVEL(AlertLevel::Warning, site.latestAlertOnAir());
}

static void test_a_short_spike_in_rise_that_is_not_sustained_does_not_raise_warning() {
  TestSite site;
  float levelCm = trickleCm(site);
  beginStorm(site, levelCm);

  // Twice the threshold rate, but one measurement short of the sustain time; then the
  // level holds for longer than the rate-of-rise window.
  levelCm = riseSteadily(site, levelCm, 2.0f * fastRiseCmPerMin(), sustainMeasurements(site) - 1);
  holdSteady(site, 2 * windowMeasurements(site));

  TEST_ASSERT_TRUE(levelCm < thresholdCm(site, AlertLevel::Watch));
  TEST_ASSERT_EQUAL_UINT(0, site.packetsOnAir(PacketKind::Alert).size());
  ASSERT_SIREN_SOUND(SirenSound::Silent, site.listenToSiren());
  TEST_ASSERT_FALSE(site.siren.isStrobeOn());
}

static void test_a_surge_that_falls_back_does_not_raise_warning() {
  TestSite site;
  const float baseCm = trickleCm(site);
  // As much in one measurement as a sustained rise at the threshold brings in its whole time.
  const float surgeCm =
      baseCm + risePerMeasurementCm(site, fastRiseCmPerMin()) * sustainMeasurements(site);
  TEST_ASSERT_TRUE(surgeCm < thresholdCm(site, AlertLevel::Watch));
  beginStorm(site, baseCm);

  measureAt(site, surgeCm);
  measureAt(site, baseCm);
  holdSteady(site, windowMeasurements(site));

  TEST_ASSERT_EQUAL_UINT(0, site.packetsOnAir(PacketKind::Alert).size());
}

static void test_a_fast_rise_that_pauses_before_the_sustain_time_does_not_raise_warning() {
  TestSite site;
  float levelCm = trickleCm(site);
  beginStorm(site, levelCm);

  for (int burst = 0; burst < 2; ++burst) {
    levelCm = riseSteadily(site, levelCm, fastRiseCmPerMin(), sustainMeasurements(site) - 1);
    holdSteady(site, 1);
  }

  TEST_ASSERT_TRUE(levelCm < thresholdCm(site, AlertLevel::Watch));
  TEST_ASSERT_EQUAL_UINT(0, site.packetsOnAir(PacketKind::Alert).size());
}

static void test_a_rise_slower_than_the_threshold_does_not_raise_warning_however_long_it_lasts() {
  TestSite site;
  const float cmPerMin =
      site.config.wadiNode.alertThresholds.warningRateOfRiseCmPerMin * 0.8f;
  float levelCm = trickleCm(site);
  beginStorm(site, levelCm);

  // It rises until the next measurement would reach the Watch threshold.
  uint32_t measurements = 0;
  while (levelCm + risePerMeasurementCm(site, cmPerMin) < thresholdCm(site, AlertLevel::Watch)) {
    levelCm = riseSteadily(site, levelCm, cmPerMin, 1);
    ++measurements;
  }

  TEST_ASSERT_TRUE(measurements > 2 * sustainMeasurements(site));
  TEST_ASSERT_EQUAL_UINT(0, site.packetsOnAir(PacketKind::Alert).size());
}

static void test_a_warning_raised_by_rate_of_rise_stays_raised_when_the_rise_stops() {
  TestSite site;
  float levelCm = trickleCm(site);
  beginStorm(site, levelCm);
  levelCm = riseSteadily(site, levelCm, fastRiseCmPerMin(), sustainMeasurements(site));

  holdSteady(site, 2 * windowMeasurements(site));

  ASSERT_ALERT_LEVEL(AlertLevel::Warning, site.latestAlertOnAir());
  ASSERT_SIREN_SOUND(SirenSound::Intermittent, site.listenToSiren());
}

// --- Rate of rise: site configuration ---

static void test_the_rate_of_rise_threshold_comes_from_the_site_configuration() {
  // A wadi committee decides this wadi must rise twice as fast before Warning.
  SiteConfig recalibrated = wadiwatch::prototypeSite();
  recalibrated.wadiNode.alertThresholds.warningRateOfRiseCmPerMin *= 2.0f;
  TestSite site(recalibrated);
  TestSite prototypeSite;

  // The same rise at both.
  beginStorm(site, trickleCm(site));
  beginStorm(prototypeSite, trickleCm(prototypeSite));
  riseSteadily(site, trickleCm(site), fastRiseCmPerMin(), sustainMeasurements(site));
  riseSteadily(prototypeSite, trickleCm(prototypeSite), fastRiseCmPerMin(),
               sustainMeasurements(prototypeSite));

  ASSERT_ALERT_LEVEL(AlertLevel::None, site.latestAlertOnAir());
  ASSERT_ALERT_LEVEL(AlertLevel::Warning, prototypeSite.latestAlertOnAir());
}

static void test_a_lower_rate_of_rise_threshold_raises_warning_on_a_slower_rise() {
  SiteConfig recalibrated = wadiwatch::prototypeSite();
  recalibrated.wadiNode.alertThresholds.warningRateOfRiseCmPerMin /= 2.0f;
  TestSite site(recalibrated);
  TestSite prototypeSite;
  const float cmPerMin = fastRiseCmPerMin() / 2.0f;

  beginStorm(site, trickleCm(site));
  beginStorm(prototypeSite, trickleCm(prototypeSite));
  riseSteadily(site, trickleCm(site), cmPerMin, sustainMeasurements(site));
  riseSteadily(prototypeSite, trickleCm(prototypeSite), cmPerMin,
               sustainMeasurements(prototypeSite));

  ASSERT_ALERT_LEVEL(AlertLevel::Warning, site.latestAlertOnAir());
  ASSERT_ALERT_LEVEL(AlertLevel::None, prototypeSite.latestAlertOnAir());
}

static void test_the_time_a_rise_must_be_sustained_comes_from_the_site_configuration() {
  // Here a rise has to last for the whole rate-of-rise window.
  SiteConfig patient = wadiwatch::prototypeSite();
  patient.wadiNode.alertThresholds.warningRateOfRiseSustainMs = patient.wadiNode.rateOfRise.windowMs;
  TestSite site(patient);
  TestSite prototypeSite;
  TEST_ASSERT_TRUE(sustainMeasurements(site) > sustainMeasurements(prototypeSite));
  float levelCm = trickleCm(site);
  beginStorm(site, levelCm);

  // What raises Warning at the prototype's sustain time is not enough here.
  levelCm = riseSteadily(site, levelCm, fastRiseCmPerMin(), sustainMeasurements(site) - 1);
  ASSERT_ALERT_LEVEL(AlertLevel::None, site.latestAlertOnAir());

  levelCm = riseSteadily(site, levelCm, fastRiseCmPerMin(), 1);

  TEST_ASSERT_TRUE(levelCm < thresholdCm(site, AlertLevel::Watch));
  ASSERT_ALERT_LEVEL(AlertLevel::Warning, site.latestAlertOnAir());
}

// After a short rise the level holds. Returns how many measurements after the last rising
// one the node first reports no rate of rise again: when the rise has left its window.
static uint32_t measurementsUntilTheRiseLeavesTheWindow(TestSite& site) {
  float levelCm = trickleCm(site);
  beginStorm(site, levelCm);
  levelCm = riseSteadily(site, levelCm, fastRiseCmPerMin(), sustainMeasurements(site) - 1);
  TEST_ASSERT_TRUE(readingsOnAir(site).back().reading.rateOfRiseMmPerMin > 0);

  uint32_t measurements = 0;
  do {
    TEST_ASSERT_TRUE_MESSAGE(measurements < 100, "the rate of rise never returned to zero");
    holdSteady(site, 1);
    ++measurements;
  } while (readingsOnAir(site).back().reading.rateOfRiseMmPerMin != 0);
  return measurements;
}

static void test_the_rate_of_rise_window_comes_from_the_site_configuration() {
  SiteConfig shortWindow = wadiwatch::prototypeSite();
  shortWindow.wadiNode.rateOfRise.windowMs =
      shortWindow.wadiNode.alertThresholds.warningRateOfRiseSustainMs;
  TestSite site(shortWindow);
  TestSite prototypeSite;
  TEST_ASSERT_TRUE(windowMeasurements(site) < windowMeasurements(prototypeSite));

  TEST_ASSERT_EQUAL_UINT32(windowMeasurements(site), measurementsUntilTheRiseLeavesTheWindow(site));
  TEST_ASSERT_EQUAL_UINT32(windowMeasurements(prototypeSite),
                           measurementsUntilTheRiseLeavesTheWindow(prototypeSite));
}

// --- Reading packets ---

static void test_in_storm_mode_every_measurement_is_sent_as_a_reading_with_level_and_rate_of_rise() {
  TestSite site;
  const WadiNodeConfig& config = site.config.wadiNode;
  // Well short of the Warning rate of rise.
  const float cmPerMin = config.alertThresholds.warningRateOfRiseCmPerMin * 0.4f;
  const float startCm = trickleCm(site);
  const uint32_t risingMeasurements = 2 * windowMeasurements(site);
  beginStorm(site, startCm);

  const float endCm = riseSteadily(site, startCm, cmPerMin, risingMeasurements);

  TEST_ASSERT_TRUE(endCm < thresholdCm(site, AlertLevel::Watch));
  const std::vector<Packet> readings = readingsOnAir(site);
  TEST_ASSERT_EQUAL_UINT(1 + risingMeasurements, readings.size());
  TEST_ASSERT_EQUAL_UINT(readings.size() * kPingsPerMeasurement, site.levelSensor.readCount());
  for (size_t i = 0; i < readings.size(); ++i) {
    const float levelCm = startCm + risePerMeasurementCm(site, cmPerMin) * static_cast<float>(i);
    TEST_ASSERT_EQUAL_UINT16(config.id, readings[i].sender);
    TEST_ASSERT_INT_WITHIN(1, toMm(levelCm), readings[i].reading.levelMm);
    // The first measurement of the storm has nothing to compare with.
    TEST_ASSERT_INT_WITHIN(1, i == 0 ? 0 : toMm(cmPerMin), readings[i].reading.rateOfRiseMmPerMin);
    // One tip of the rain gauge so far, carried in tenths of a millimetre.
    TEST_ASSERT_EQUAL_UINT16(toTenthsMm(config.rain.mmPerTip),
                             readings[i].reading.rainfallTenthsMm);
    ASSERT_ALERT_LEVEL(AlertLevel::None, readings[i].reading.alertLevel);
    TEST_ASSERT_TRUE(readings[i].reading.stormMode);
  }
  TEST_ASSERT_EQUAL_UINT(0, site.packetsOnAir(PacketKind::Alert).size());
}

static void test_a_falling_level_is_reported_as_a_negative_rate_of_rise() {
  TestSite site;
  const float cmPerMin = -site.config.wadiNode.alertThresholds.warningRateOfRiseCmPerMin * 0.4f;
  const float startCm = lowLevelCm(site);
  beginStorm(site, startCm);

  riseSteadily(site, startCm, cmPerMin, windowMeasurements(site));

  TEST_ASSERT_INT_WITHIN(1, toMm(cmPerMin), readingsOnAir(site).back().reading.rateOfRiseMmPerMin);
}

static void test_a_reading_follows_the_alert_its_measurement_raised_and_carries_the_alert_level() {
  TestSite site;
  const float levelCm = (thresholdCm(site, AlertLevel::Warning) +
                         thresholdCm(site, AlertLevel::Evacuate)) /
                        2.0f;

  // No rain: the measurement that finds the flood is the one that starts storm mode.
  measureAt(site, levelCm);
  // The alert goes out first, as many times as configured; the reading waits behind it.
  const wadiwatch::AlertRepeatConfig& repeat = site.config.wadiNode.alertRepeat;
  TEST_ASSERT_EQUAL_UINT(1, wadiNodePackets(site).size());
  site.runMs(static_cast<uint64_t>(repeat.transmissions - 1) * repeat.maxSpacingMs);

  const std::vector<Packet> packets = wadiNodePackets(site);
  TEST_ASSERT_EQUAL_UINT(2, packets.size());
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PacketKind::Alert),
                          static_cast<uint8_t>(packets[0].kind));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PacketKind::Reading),
                          static_cast<uint8_t>(packets[1].kind));
  ASSERT_ALERT_LEVEL(AlertLevel::Warning, packets[1].reading.alertLevel);
  TEST_ASSERT_INT_WITHIN(1, toMm(levelCm), packets[1].reading.levelMm);
  TEST_ASSERT_EQUAL_UINT16(0, packets[1].reading.rainfallTenthsMm);
  TEST_ASSERT_TRUE(packets[1].reading.stormMode);
}

static void test_readings_stop_when_the_node_returns_to_dry_mode() {
  TestSite site;
  const wadiwatch::OperatingModeConfig& mode = site.config.wadiNode.mode;
  beginStorm(site, lowLevelCm(site));
  // The rain does not come back: after the quiet period the node is in dry mode again.
  site.runMs(static_cast<uint64_t>(mode.stormQuietMs) + mode.dryMeasureIntervalMs);
  const size_t readingsInTheStorm = readingsOnAir(site).size();
  const size_t pingsBefore = site.levelSensor.readCount();
  TEST_ASSERT_TRUE(readingsInTheStorm > 0);

  site.runMs(3ull * mode.dryMeasureIntervalMs);

  TEST_ASSERT_TRUE(site.levelSensor.readCount() > pingsBefore);
  TEST_ASSERT_EQUAL_UINT(readingsInTheStorm, readingsOnAir(site).size());
}

static void test_a_measurement_that_gives_no_level_sends_no_reading() {
  TestSite site;
  beginStorm(site, lowLevelCm(site));
  const size_t readingsBefore = readingsOnAir(site).size();

  for (size_t i = 0; i < kPingsPerMeasurement; ++i) {
    site.levelSensor.scriptPingDistanceCm(impossiblyNearCm(site));
  }
  runUntilMeasured(site);

  TEST_ASSERT_EQUAL_UINT(0, site.levelSensor.scriptedPingsLeft());
  TEST_ASSERT_EQUAL_UINT(readingsBefore, readingsOnAir(site).size());
}

// --- A level the node could not measure again ---

// A level between the Warning and the Evacuate threshold.
static float warningLevelCm(const TestSite& site) {
  return (thresholdCm(site, AlertLevel::Warning) + thresholdCm(site, AlertLevel::Evacuate)) / 2.0f;
}

static void test_what_a_node_sends_carries_a_level_it_has_just_measured_as_such() {
  TestSite site;
  measureAt(site, warningLevelCm(site));
  site.runMs(site.config.wadiNode.heartbeatIntervalMs + kMinuteMs);

  const std::vector<Packet> alerts = site.packetsOnAir(PacketKind::Alert, site.config.wadiNode.id);
  TEST_ASSERT_TRUE(alerts.size() > 1);
  for (const Packet& alert : alerts) TEST_ASSERT_FALSE(alert.alert.levelRepeated);
  TEST_ASSERT_FALSE(readingsOnAir(site).empty());
  for (const Packet& reading : readingsOnAir(site)) {
    TEST_ASSERT_FALSE(reading.reading.levelRepeated);
  }
  TEST_ASSERT_FALSE(wadiNodeHeartbeats(site).empty());
  for (const Packet& heartbeat : wadiNodeHeartbeats(site)) {
    TEST_ASSERT_TRUE(heartbeat.heartbeat.hasLastLevel);
    TEST_ASSERT_FALSE(heartbeat.heartbeat.lastLevelRepeated);
  }
}

static void test_an_alert_refresh_sent_while_the_sensor_gives_no_level_says_that_its_level_is_repeated() {
  TestSite site;
  const WadiNodeConfig& config = site.config.wadiNode;
  measureAt(site, warningLevelCm(site));
  site.runMs(2ull * config.alertRefreshIntervalMs);
  const Packet measured = site.packetsOnAir(PacketKind::Alert, config.id).back();
  TEST_ASSERT_FALSE(measured.alert.levelRepeated);

  // The sensor stops giving an echo. The node keeps the level it last measured and
  // goes on sending its alert refresh.
  site.levelSensor.setEchoing(false);
  site.runMs(3ull * config.alertRefreshIntervalMs);

  const Packet repeated = site.packetsOnAir(PacketKind::Alert, config.id).back();
  TEST_ASSERT_TRUE(repeated.counter > measured.counter);
  TEST_ASSERT_EQUAL_UINT16(measured.alert.levelMm, repeated.alert.levelMm);
  TEST_ASSERT_TRUE(repeated.alert.levelRepeated);

  // The sensor works again: the next alert refresh carries a level just measured.
  site.levelSensor.setEchoing(true);
  site.runMs(2ull * config.alertRefreshIntervalMs);
  TEST_ASSERT_FALSE(site.packetsOnAir(PacketKind::Alert, config.id).back().alert.levelRepeated);
}

static void test_a_heartbeat_sent_while_the_sensor_gives_no_level_says_that_its_level_is_repeated() {
  TestSite site;
  const WadiNodeConfig& config = site.config.wadiNode;
  measureAt(site, lowLevelCm(site));

  // The sensor dies on a dry day. The hourly heartbeat still carries the last level
  // the node measured, and says that the node is no longer measuring it.
  site.levelSensor.setEchoing(false);
  site.runMs(config.heartbeatIntervalMs + kMinuteMs);

  const std::vector<Packet> heartbeats = wadiNodeHeartbeats(site);
  TEST_ASSERT_EQUAL_UINT(1, heartbeats.size());
  TEST_ASSERT_TRUE(heartbeats[0].heartbeat.hasLastLevel);
  TEST_ASSERT_TRUE(heartbeats[0].heartbeat.lastLevelRepeated);
}

// --- Mounting height and temperature ---

static void test_the_level_is_the_mounting_height_minus_the_measured_distance() {
  // The same wadi with the sensor on a higher bridge.
  SiteConfig higher = wadiwatch::prototypeSite();
  higher.wadiNode.mountingHeightCm += 100.0f;
  TestSite higherSite(higher);
  TestSite prototypeSite;
  const float levelCm = lowLevelCm(prototypeSite);

  beginStorm(higherSite, levelCm);
  beginStorm(prototypeSite, levelCm);

  TEST_ASSERT_INT_WITHIN(1, toMm(levelCm), readingsOnAir(higherSite).back().reading.levelMm);
  TEST_ASSERT_INT_WITHIN(1, toMm(levelCm), readingsOnAir(prototypeSite).back().reading.levelMm);
}

static void test_with_a_temperature_reading_the_level_is_right_in_hot_and_in_freezing_air() {
  for (float airC : {kHotAirC, kFreezingAirC}) {
    TestSite site;
    const float levelCm = lowLevelCm(site);
    // Uncorrected, this air would put the level out by more than a centimetre.
    TEST_ASSERT_TRUE(std::fabs(uncorrectedLevelCm(site, levelCm, airC) - levelCm) > 1.0f);
    site.levelSensor.setAirTemperatureC(airC);
    site.temperatureSensor.setTemperatureC(airC);

    beginStorm(site, levelCm);

    TEST_ASSERT_INT_WITHIN(1, toMm(levelCm), readingsOnAir(site).back().reading.levelMm);
  }
}

static void test_without_a_temperature_reading_the_level_is_left_uncorrected() {
  TestSite site;
  const float levelCm = lowLevelCm(site);
  site.levelSensor.setAirTemperatureC(kHotAirC);
  // The thermometer has stopped answering.
  site.temperatureSensor.setTemperatureC(kHotAirC);
  site.temperatureSensor.setUnavailable();

  beginStorm(site, levelCm);

  // The node still measures; in hot air the water looks nearer, so the level reads high.
  const float expectedCm = uncorrectedLevelCm(site, levelCm, kHotAirC);
  TEST_ASSERT_TRUE(expectedCm > levelCm + 1.0f);
  TEST_ASSERT_INT_WITHIN(1, toMm(expectedCm), readingsOnAir(site).back().reading.levelMm);
}

static void test_the_temperature_correction_keeps_hot_air_from_raising_watch() {
  TestSite withThermometer;
  TestSite withoutThermometer;
  // A level just short of the Watch threshold that hot air, uncorrected, carries over it.
  const float watchCm = thresholdCm(withThermometer, AlertLevel::Watch);
  const float levelCm = watchCm - 2.0f;
  TEST_ASSERT_TRUE(uncorrectedLevelCm(withThermometer, levelCm, kHotAirC) >= watchCm);
  withThermometer.levelSensor.setAirTemperatureC(kHotAirC);
  withThermometer.temperatureSensor.setTemperatureC(kHotAirC);
  withoutThermometer.levelSensor.setAirTemperatureC(kHotAirC);

  measureAt(withThermometer, levelCm);
  measureAt(withoutThermometer, levelCm);

  ASSERT_ALERT_LEVEL(AlertLevel::None, withThermometer.latestAlertOnAir());
  ASSERT_ALERT_LEVEL(AlertLevel::Watch, withoutThermometer.latestAlertOnAir());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_single_outlier_pings_do_not_change_the_measured_level_or_raise_an_alert);
  RUN_TEST(test_outlier_pings_are_not_counted_as_discarded_readings);
  RUN_TEST(test_in_dry_mode_an_outlier_ping_does_not_wake_the_node_into_storm_mode);
  RUN_TEST(test_readings_outside_the_possible_range_are_discarded_and_counted_in_the_heartbeat);
  RUN_TEST(test_a_ping_without_an_echo_is_counted_as_a_discarded_reading);
  RUN_TEST(test_each_heartbeat_counts_the_readings_discarded_since_the_one_before);
  RUN_TEST(test_a_fouled_sensor_shows_up_in_the_heartbeat_and_never_as_a_flood);
  RUN_TEST(test_the_discard_count_in_a_heartbeat_stops_at_its_largest_value);
  RUN_TEST(test_a_fast_sustained_rise_at_a_low_level_raises_warning_by_rate_of_rise_alone);
  RUN_TEST(test_a_rise_at_exactly_the_threshold_rate_raises_warning);
  RUN_TEST(test_a_short_spike_in_rise_that_is_not_sustained_does_not_raise_warning);
  RUN_TEST(test_a_surge_that_falls_back_does_not_raise_warning);
  RUN_TEST(test_a_fast_rise_that_pauses_before_the_sustain_time_does_not_raise_warning);
  RUN_TEST(test_a_rise_slower_than_the_threshold_does_not_raise_warning_however_long_it_lasts);
  RUN_TEST(test_a_warning_raised_by_rate_of_rise_stays_raised_when_the_rise_stops);
  RUN_TEST(test_the_rate_of_rise_threshold_comes_from_the_site_configuration);
  RUN_TEST(test_a_lower_rate_of_rise_threshold_raises_warning_on_a_slower_rise);
  RUN_TEST(test_the_time_a_rise_must_be_sustained_comes_from_the_site_configuration);
  RUN_TEST(test_the_rate_of_rise_window_comes_from_the_site_configuration);
  RUN_TEST(test_in_storm_mode_every_measurement_is_sent_as_a_reading_with_level_and_rate_of_rise);
  RUN_TEST(test_a_falling_level_is_reported_as_a_negative_rate_of_rise);
  RUN_TEST(test_a_reading_follows_the_alert_its_measurement_raised_and_carries_the_alert_level);
  RUN_TEST(test_readings_stop_when_the_node_returns_to_dry_mode);
  RUN_TEST(test_a_measurement_that_gives_no_level_sends_no_reading);
  RUN_TEST(test_what_a_node_sends_carries_a_level_it_has_just_measured_as_such);
  RUN_TEST(test_an_alert_refresh_sent_while_the_sensor_gives_no_level_says_that_its_level_is_repeated);
  RUN_TEST(test_a_heartbeat_sent_while_the_sensor_gives_no_level_says_that_its_level_is_repeated);
  RUN_TEST(test_the_level_is_the_mounting_height_minus_the_measured_distance);
  RUN_TEST(test_with_a_temperature_reading_the_level_is_right_in_hot_and_in_freezing_air);
  RUN_TEST(test_without_a_temperature_reading_the_level_is_left_uncorrected);
  RUN_TEST(test_the_temperature_correction_keeps_hot_air_from_raising_watch);
  return UNITY_END();
}
