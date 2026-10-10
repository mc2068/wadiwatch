#include "wadiwatch/wadi_node.h"

#include "wadiwatch/alert_engine.h"

namespace wadiwatch {

WadiNode::WadiNode(const WadiNodeConfig& config,
                   Clock& clock,
                   SleepControl& sleep,
                   LevelSensor& sensor,
                   RainGauge& rainGauge,
                   NodeHealth& health,
                   Radio& radio)
    : config_(config),
      clock_(clock),
      sleep_(sleep),
      sensor_(sensor),
      rainGauge_(rainGauge),
      health_(health),
      radio_(radio),
      mode_(config.mode),
      rain_(config.rain, config.alertThresholds.watchRainWindowMs) {}

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
  packet.sender = config_.id;
  packet.alertLevel = alertLevel_;
  uint8_t frame[kMaxPacketSize];
  const size_t length = encodePacket(packet, frame, sizeof(frame));
  if (length > 0) radio_.send(frame, length);
}

void WadiNode::sendHeartbeat() {
  Packet packet = Packet();
  packet.kind = PacketKind::Heartbeat;
  packet.sender = config_.id;
  packet.heartbeat.batteryMillivolts = health_.batteryMillivolts();
  packet.heartbeat.signalStrengthDbm = health_.signalStrengthDbm();
  packet.heartbeat.tamper = health_.tamperDetected();
  packet.heartbeat.hasLastLevel = hasLevel_;
  // Rounded to the nearest centimetre.
  packet.heartbeat.lastLevelCm =
      static_cast<int16_t>(levelCm_ < 0 ? levelCm_ - 0.5f : levelCm_ + 0.5f);
  uint8_t frame[kMaxPacketSize];
  const size_t length = encodePacket(packet, frame, sizeof(frame));
  if (length > 0) radio_.send(frame, length);
}

void WadiNode::sleepUntilNextDue(uint64_t nowMs) {
  const uint64_t nextDueAtMs =
      nextMeasurementAtMs_ < nextHeartbeatAtMs_ ? nextMeasurementAtMs_ : nextHeartbeatAtMs_;
  if (nextDueAtMs > nowMs) sleep_.requestSleepMs(nextDueAtMs - nowMs);
}

}  // namespace wadiwatch
