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
                   PersistentStorage& storage)
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
      upstream_(config.upstream) {}

void WadiNode::tick() {
  const uint64_t now = clock_.nowMs();
  if (!started_) {
    started_ = true;
    nextMeasurementAtMs_ = now;
    nextHeartbeatAtMs_ = now + config_.heartbeatIntervalMs;
    // The receiver stays off except where switchReceiver() turns it on.
    radio_.setListening(false);
  }

  // A node with an alert to send again does not go back to dry mode.
  if (hasAlertToResend()) mode_.onAlertInForce(now);

  const OperatingMode modeBefore = mode_.mode();
  countRain(now);
  // Rain that starts a storm does not wait for the next dry-mode measurement.
  if (modeBefore == OperatingMode::Dry && mode_.mode() == OperatingMode::Storm) {
    nextMeasurementAtMs_ = now;
  }

  if (now >= nextMeasurementAtMs_) {
    measure(now);
    nextMeasurementAtMs_ = now + mode_.measureIntervalMs();
    upstream_.onMeasurement(now);
  }

  handleReceivedFrames();
  raiseAlertLevel(now);
  resendAlert(now);
  switchReceiver(now);

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

// Takes in what the receiver has heard. The codec is the only way in: a frame that
// is forged, foreign or replayed is dropped here.
void WadiNode::handleReceivedFrames() {
  if (!receiverOn_) return;
  uint8_t frame[kMaxPacketSize];
  size_t length;
  while ((length = radio_.receive(frame, sizeof(frame))) > 0) {
    Packet packet;
    if (!codec_.decode(frame, length, packet)) continue;
    upstream_.onPacket(packet);
  }
}

// The alert engine decides; this keeps the highest alert level it has called for.
// Alert levels only escalate automatically; lowering one takes an all clear.
void WadiNode::raiseAlertLevel(uint64_t nowMs) {
  // Until the first measurement the level is that of a dry bed. The rate of rise
  // is not measured yet.
  const AlertInputs inputs = {levelCm_, 0.0f, rain_.rainMm(nowMs), upstream_.alertLevel(),
                              alertLevel_};
  const AlertLevel evaluated = evaluateAlertLevel(inputs, config_.alertThresholds);
  if (evaluated <= alertLevel_) return;
  alertLevel_ = evaluated;
  announceAlert(nowMs);
}

// Watch is announced once. From Warning up the alert is sent again and again, so that
// a receiver that missed it, and the node downstream, still hear it.
bool WadiNode::hasAlertToResend() const { return alertLevel_ >= AlertLevel::Warning; }

void WadiNode::announceAlert(uint64_t nowMs) {
  sendAlert();
  nextAlertResendAtMs_ = nowMs + config_.alertResendIntervalMs;
}

// Each re-send is a new packet with its own counter: receivers accept it as a refresh.
void WadiNode::resendAlert(uint64_t nowMs) {
  if (hasAlertToResend() && nowMs >= nextAlertResendAtMs_) announceAlert(nowMs);
}

// The upstream link says when the receiver is worth its energy.
void WadiNode::switchReceiver(uint64_t nowMs) {
  const bool wanted = upstream_.wantsReceiver(nowMs, mode_.mode(), alertLevel_);
  if (wanted == receiverOn_) return;
  if (wanted) {
    // Only what arrives from now on counts as heard: drop whatever the radio still
    // holds from before, such as frames that came in before the node first switched it off.
    uint8_t stale[kMaxPacketSize];
    while (radio_.receive(stale, sizeof(stale)) > 0) {
    }
  }
  receiverOn_ = wanted;
  radio_.setListening(wanted);
}

void WadiNode::sendAlert() {
  Packet packet = Packet();
  packet.kind = PacketKind::Alert;
  packet.alert.alertLevel = alertLevel_;
  // The rate of rise is not measured yet.
  packet.alert.levelMm = levelMm(levelCm_);
  send(packet);
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

// Everything this node transmits leaves through here: the codec stamps the
// packet with the node's identifier and next counter, and signs it.
void WadiNode::send(Packet& packet) {
  uint8_t frame[kMaxPacketSize];
  const size_t length = codec_.encode(packet, frame, sizeof(frame));
  if (length > 0) radio_.send(frame, length);
}

void WadiNode::sleepUntilNextDue(uint64_t nowMs) {
  uint64_t nextDueAtMs =
      nextMeasurementAtMs_ < nextHeartbeatAtMs_ ? nextMeasurementAtMs_ : nextHeartbeatAtMs_;
  if (hasAlertToResend() && nextAlertResendAtMs_ < nextDueAtMs) {
    nextDueAtMs = nextAlertResendAtMs_;
  }
  // The end of an open receive window is due work too: the receiver has to be switched off.
  const uint64_t windowEndsAtMs = upstream_.windowEndsAtMs();
  if (receiverOn_ && windowEndsAtMs > nowMs && windowEndsAtMs < nextDueAtMs) {
    nextDueAtMs = windowEndsAtMs;
  }
  if (nextDueAtMs > nowMs) sleep_.requestSleepMs(nextDueAtMs - nowMs);
}

}  // namespace wadiwatch
