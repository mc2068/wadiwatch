#include "wadiwatch/wadi_node.h"

#include "wadiwatch/alert_engine.h"

namespace wadiwatch {

namespace {

constexpr float kMmPerCm = 10.0f;
constexpr float kMaxLevelMm = 65535.0f;

// A level as packets carry it: whole millimetres above the wadi bed. A reading
// below the bed (sensor noise over a dry wadi) travels as zero.
uint16_t levelMm(float levelCm) {
  const float mm = levelCm * kMmPerCm + 0.5f;
  if (!(mm > 0.0f)) return 0;
  if (mm >= kMaxLevelMm) return static_cast<uint16_t>(kMaxLevelMm);
  return static_cast<uint16_t>(mm);
}

}  // namespace

WadiNode::WadiNode(const WadiNodeConfig& config,
                   const SiteAuth& auth,
                   Clock& clock,
                   SleepControl& sleep,
                   LevelSensor& sensor,
                   RainGauge& rainGauge,
                   NodeHealth& health,
                   Radio& radio,
                   PersistentStorage& storage,
                   RandomSource& random)
    : config_(config),
      clock_(clock),
      sleep_(sleep),
      sensor_(sensor),
      rainGauge_(rainGauge),
      health_(health),
      radio_(radio),
      codec_(auth, config.id, storage),
      mode_(config.mode),
      rain_(config.rain, config.alertThresholds.watchRainWindowMs),
      alertRepeater_(config.alertRepeat, random) {}

void WadiNode::tick() {
  const uint64_t now = clock_.nowMs();
  if (!started_) {
    started_ = true;
    nextMeasurementAtMs_ = now;
    nextHeartbeatAtMs_ = now + config_.heartbeatIntervalMs;
  }

  const OperatingMode modeBefore = mode_.mode();
  countRain(now);
  // Rain that starts a storm does not wait for the next dry-mode measurement.
  if (modeBefore == OperatingMode::Dry && mode_.mode() == OperatingMode::Storm) {
    nextMeasurementAtMs_ = now;
  }

  if (now >= nextMeasurementAtMs_) {
    measure(now);
    nextMeasurementAtMs_ = now + mode_.measureIntervalMs();
  }

  raiseAlertLevel(now);
  repeatAlert(now);

  if (now >= nextHeartbeatAtMs_) {
    sendHeartbeat();
    nextHeartbeatAtMs_ = now + config_.heartbeatIntervalMs;
  }

  sleepUntilNextDue(now);
}

void WadiNode::countRain(uint64_t nowMs) {
  const uint32_t tips = rainGauge_.tipCount();
  const uint32_t newTips = tips - tipsCounted_;
  if (newTips == 0) return;
  tipsCounted_ = tips;
  rain_.addTips(nowMs, newTips);
  mode_.onRain(nowMs);
}

void WadiNode::measure(uint64_t nowMs) {
  float distanceCm = 0;
  if (!sensor_.readDistanceCm(distanceCm)) return;

  levelCm_ = config_.mountingHeightCm - distanceCm;
  hasLevel_ = true;
  const float watchLevelCm = levelThresholdCm(config_.alertThresholds, AlertLevel::Watch);
  mode_.onLevel(nowMs, levelCm_ >= watchLevelCm);
}

// The alert engine decides; this keeps the highest alert level it has called for.
// Alert levels only escalate automatically; lowering one takes an all clear.
void WadiNode::raiseAlertLevel(uint64_t nowMs) {
  // Until the first measurement the level is that of a dry bed. Rate of rise
  // and the upstream node are not measured yet.
  const AlertInputs inputs = {levelCm_, 0.0f, rain_.rainMm(nowMs), AlertLevel::None};
  const AlertLevel evaluated = evaluateAlertLevel(inputs, config_.alertThresholds);
  if (evaluated <= alertLevel_) return;
  alertLevel_ = evaluated;
  sendAlert();
}

void WadiNode::sendAlert() {
  Packet packet = Packet();
  packet.kind = PacketKind::Alert;
  packet.alert.alertLevel = alertLevel_;
  // The rate of rise is not measured yet.
  packet.alert.levelMm = levelMm(levelCm_);
  // Every alert goes out several times. The further transmissions are these same
  // bytes: a receiver acts on the first copy it hears and refuses the rest.
  uint8_t frame[kMaxPacketSize];
  const size_t length = send(packet, frame, sizeof(frame));
  alertRepeater_.start(frame, length, clock_.nowMs());
}

void WadiNode::repeatAlert(uint64_t nowMs) {
  size_t length = 0;
  const uint8_t* frame = alertRepeater_.takeDue(nowMs, length);
  if (frame != nullptr) transmit(frame, length);
}

void WadiNode::sendHeartbeat() {
  Packet packet = Packet();
  packet.kind = PacketKind::Heartbeat;
  packet.heartbeat.batteryMillivolts = health_.batteryMillivolts();
  packet.heartbeat.signalStrengthDbm = health_.signalStrengthDbm();
  packet.heartbeat.tamper = health_.tamperDetected();
  packet.heartbeat.hasLastLevel = hasLevel_;
  packet.heartbeat.lastLevelMm = levelMm(levelCm_);
  send(packet);
}

// Every packet this node sends leaves through here: the codec stamps the
// packet with the node's identifier and next counter, and signs it.
void WadiNode::send(Packet& packet) {
  uint8_t frame[kMaxPacketSize];
  send(packet, frame, sizeof(frame));
}

// The same, for a caller that keeps the frame to transmit it again. Returns the
// frame's length, or 0 if nothing was sent.
size_t WadiNode::send(Packet& packet, uint8_t* frame, size_t capacity) {
  const size_t length = codec_.encode(packet, frame, capacity);
  transmit(frame, length);
  return length;
}

// The only place this node touches the radio's transmitter.
void WadiNode::transmit(const uint8_t* frame, size_t length) {
  if (length > 0) radio_.send(frame, length);
}

void WadiNode::sleepUntilNextDue(uint64_t nowMs) {
  uint64_t nextDueAtMs =
      nextMeasurementAtMs_ < nextHeartbeatAtMs_ ? nextMeasurementAtMs_ : nextHeartbeatAtMs_;
  if (alertRepeater_.pending() && alertRepeater_.dueAtMs() < nextDueAtMs) {
    nextDueAtMs = alertRepeater_.dueAtMs();
  }
  if (nextDueAtMs > nowMs) sleep_.requestSleepMs(nextDueAtMs - nowMs);
}

}  // namespace wadiwatch
