#include "wadiwatch/wadi_node.h"

namespace wadiwatch {

WadiNode::WadiNode(const WadiNodeConfig& config, const SiteAuth& auth, Clock& clock,
                   LevelSensor& sensor, Radio& radio, PersistentStorage& storage)
    : config_(config),
      clock_(clock),
      sensor_(sensor),
      radio_(radio),
      codec_(auth, config.id, storage) {}

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
  Packet packet = Packet();
  packet.kind = PacketKind::Alert;
  packet.alert.alertLevel = alertLevel_;
  uint8_t frame[kMaxPacketSize];
  const size_t length = codec_.encode(packet, frame, sizeof(frame));
  if (length > 0) radio_.send(frame, length);
}

}  // namespace wadiwatch
