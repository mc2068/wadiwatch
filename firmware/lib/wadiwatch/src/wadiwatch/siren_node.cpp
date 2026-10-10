#include "wadiwatch/siren_node.h"

namespace wadiwatch {

SirenNode::SirenNode(const SirenNodeConfig& config,
                     const SiteAuth& auth,
                     Clock& clock,
                     Radio& radio,
                     SirenOutput& output,
                     PersistentStorage& storage)
    : config_(config),
      clock_(clock),
      radio_(radio),
      output_(output),
      codec_(auth, config.id, storage) {}

void SirenNode::tick() {
  const uint64_t nowMs = clock_.nowMs();
  handleReceivedFrames(nowMs);
  driveOutput(nowMs);
}

void SirenNode::handleReceivedFrames(uint64_t nowMs) {
  uint8_t frame[kMaxPacketSize];
  size_t length;
  while ((length = radio_.receive(frame, sizeof(frame))) > 0) {
    Packet packet;
    // The codec is the only way in: a frame that is forged, foreign or replayed is dropped here.
    if (!codec_.decode(frame, length, packet)) continue;
    if (packet.kind != PacketKind::Alert) continue;
    // A repeat of the alert in force must not restart the pattern, and a lower
    // alert level must not quieten the siren.
    if (packet.alert.alertLevel <= alertLevel_) continue;
    alertLevel_ = packet.alert.alertLevel;
    if (alertLevel_ == AlertLevel::Warning) intermittentStartedAtMs_ = nowMs;
  }
}

void SirenNode::driveOutput(uint64_t nowMs) {
  bool siren = false;
  bool strobe = false;
  if (alertLevel_ == AlertLevel::Evacuate) {
    siren = true;
    strobe = true;
  } else if (alertLevel_ == AlertLevel::Warning) {
    strobe = true;
    const uint64_t periodMs =
        static_cast<uint64_t>(config_.intermittentOnMs) + config_.intermittentOffMs;
    // The pattern starts with the siren sounding. A pattern with no duration
    // configured must not leave a Warning silent.
    siren = periodMs == 0 ||
            (nowMs - intermittentStartedAtMs_) % periodMs < config_.intermittentOnMs;
  }

  if (siren != sirenOn_) {
    sirenOn_ = siren;
    output_.setSiren(siren);
  }
  if (strobe != strobeOn_) {
    strobeOn_ = strobe;
    output_.setStrobe(strobe);
  }
}

}  // namespace wadiwatch
