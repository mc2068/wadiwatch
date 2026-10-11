#include "wadiwatch/upstream_link.h"

namespace wadiwatch {

UpstreamLink::UpstreamLink(const UpstreamConfig& config) : config_(config) {}

void UpstreamLink::onMeasurement(uint64_t nowMs) {
  windowEndsAtMs_ = nowMs + config_.receiveWindowMs;
}

bool UpstreamLink::wantsReceiver(uint64_t nowMs, OperatingMode mode, AlertLevel alertLevel) const {
  if (config_.node == kNoNode) return false;
  if (mode != OperatingMode::Storm) return false;
  if (alertLevel >= AlertLevel::Evacuate) return false;
  if (alertLevel == AlertLevel::Warning) return true;
  return nowMs < windowEndsAtMs_;
}

void UpstreamLink::onPacket(const Packet& packet, uint64_t nowMs) {
  if (config_.node == kNoNode) return;
  if (packet.sender != config_.node) return;
  if (packet.kind != PacketKind::Alert) return;
  alertLevel_ = packet.alert.alertLevel;
  heardAtMs_ = nowMs;
}

AlertLevel UpstreamLink::alertLevel(uint64_t nowMs) const {
  if (nowMs - heardAtMs_ > config_.alertRefreshTimeoutMs) return AlertLevel::None;
  return alertLevel_;
}

}  // namespace wadiwatch
