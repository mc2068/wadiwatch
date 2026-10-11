#include "wadiwatch/wadi_node.h"

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
  if (levelCm >= config_.warningLevelCm && alertLevel_ < AlertLevel::Warning) {
    alertLevel_ = AlertLevel::Warning;
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
