#include "wadiwatch/wadi_node.h"

#include "wadiwatch/alert_engine.h"

namespace wadiwatch {

WadiNode::WadiNode(const WadiNodeConfig& config, Clock& clock, LevelSensor& sensor, Radio& radio)
    : config_(config), clock_(clock), sensor_(sensor), radio_(radio) {}

void WadiNode::tick() {
  const uint64_t now = clock_.nowMs();
  if (now < nextMeasurementAtMs_) return;
  nextMeasurementAtMs_ = now + config_.measureIntervalMs;
  measure();
}

void WadiNode::measure() {
  float distanceCm = 0;
  if (!sensor_.readDistanceCm(distanceCm)) return;

  const float levelCm = config_.mountingHeightCm - distanceCm;
  // Rate of rise, rainfall intensity and the upstream node are not measured yet.
  const AlertInputs inputs = {levelCm, 0.0f, 0.0f, AlertLevel::None};
  const AlertLevel evaluated = evaluateAlertLevel(inputs, config_.alertThresholds);
  // Alert levels only escalate automatically; lowering one takes an all clear.
  if (evaluated > alertLevel_) {
    alertLevel_ = evaluated;
    sendAlert();
  }
}

void WadiNode::sendAlert() {
  const Packet packet = {PacketKind::Alert, config_.id, alertLevel_};
  uint8_t frame[kMaxPacketSize];
  const size_t length = encodePacket(packet, frame, sizeof(frame));
  if (length > 0) radio_.send(frame, length);
}

}  // namespace wadiwatch
