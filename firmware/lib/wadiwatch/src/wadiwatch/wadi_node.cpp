#include "wadiwatch/wadi_node.h"

#include "wadiwatch/alert_engine.h"
#include "wadiwatch/level_measurement.h"

namespace wadiwatch {

namespace {

constexpr float kMmPerCm = 10.0f;
constexpr float kMaxLevelMm = 65535.0f;
constexpr float kMaxRateMmPerMin = 32767.0f;
constexpr float kTenthsPerMm = 10.0f;
constexpr float kMaxRainTenthsMm = 65535.0f;
constexpr uint8_t kMaxDiscardedReadings = 255;

// A level as packets carry it: whole millimetres above the wadi bed. A reading
// below the bed (sensor noise over a dry wadi) travels as zero.
uint16_t levelMm(float levelCm) {
  const float mm = levelCm * kMmPerCm + 0.5f;
  if (!(mm > 0.0f)) return 0;
  if (mm >= kMaxLevelMm) return static_cast<uint16_t>(kMaxLevelMm);
  return static_cast<uint16_t>(mm);
}

// A rate of rise as packets carry it: whole millimetres per minute, negative
// while the level falls.
int16_t rateMmPerMin(float rateCmPerMin) {
  const float mm = rateCmPerMin * kMmPerCm;
  if (mm >= kMaxRateMmPerMin) return static_cast<int16_t>(kMaxRateMmPerMin);
  if (mm <= -kMaxRateMmPerMin) return static_cast<int16_t>(-kMaxRateMmPerMin);
  return static_cast<int16_t>(mm < 0.0f ? mm - 0.5f : mm + 0.5f);
}

// Rainfall as packets carry it: tenths of a millimetre.
uint16_t rainTenthsMm(float rainMm) {
  const float tenths = rainMm * kTenthsPerMm + 0.5f;
  if (!(tenths > 0.0f)) return 0;
  if (tenths >= kMaxRainTenthsMm) return static_cast<uint16_t>(kMaxRainTenthsMm);
  return static_cast<uint16_t>(tenths);
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
                   RandomSource& random,
                   TemperatureSensor* temperature)
    : config_(config),
      clock_(clock),
      sleep_(sleep),
      sensor_(sensor),
      rainGauge_(rainGauge),
      health_(health),
      radio_(radio),
      temperature_(temperature),
      codec_(auth, config.id, storage),
      mode_(config.mode),
      rain_(config.rain, config.alertThresholds.watchRainWindowMs),
      rise_(config.rateOfRise),
      alertRepeater_(config.alertRepeat, random),
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
  if (hasAlertToRefresh()) mode_.onAlertInForce(now);

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

  handleReceivedFrames(now);
  raiseAlertLevel(now);
  refreshAlert(now);
  repeatAlert(now);

  // While an alert still has transmissions to come, nothing else leaves this node.
  // A receiver refuses every packet older than the last one it accepted from the
  // same sender, so a reading or a heartbeat heard in between would make it refuse
  // the alert's further transmissions: the alert would be lost just when its first
  // transmission was. What is held back goes out behind the last transmission, a
  // few seconds late.
  if (!alertRepeater_.pending()) {
    // The reading carries the alert level its measurement raised.
    if (readingDue_) sendReading(now);
    if (now >= nextHeartbeatAtMs_) {
      sendHeartbeat();
      nextHeartbeatAtMs_ = now + config_.heartbeatIntervalMs;
    }
  }

  switchReceiver(now);
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

// One measurement: several pings of the level sensor, filtered into one level.
void WadiNode::measure(uint64_t nowMs) {
  AirTemperature air = {false, 0.0f};
  if (temperature_ != nullptr) air.available = temperature_->readTemperatureC(air.celsius);
  Ping pings[kPingsPerMeasurement];
  for (Ping& ping : pings) {
    ping.distanceCm = 0.0f;
    ping.echoed = sensor_.readDistanceCm(ping.distanceCm);
  }
  const LevelMeasurement measurement =
      measureLevel(pings, air, config_.mountingHeightCm, config_.levelSensor);

  discardedReadings_ = measurement.discardedPings >= kMaxDiscardedReadings - discardedReadings_
                           ? kMaxDiscardedReadings
                           : static_cast<uint8_t>(discardedReadings_ + measurement.discardedPings);
  // Too few usable pings: the node keeps the level it last measured, and says in
  // whatever it sends that it is repeating it.
  levelMeasured_ = measurement.hasLevel;
  if (!measurement.hasLevel) return;

  levelCm_ = measurement.levelCm;
  hasLevel_ = true;
  const float watchLevelCm = levelThresholdCm(config_.alertThresholds, AlertLevel::Watch);
  mode_.onLevel(nowMs, levelCm_ >= watchLevelCm);

  // The rate of rise follows storm-mode measurements only; each of them is
  // also sent as a reading.
  if (mode_.mode() == OperatingMode::Storm) {
    rise_.addMeasurement(nowMs, levelCm_);
    readingDue_ = true;
  } else {
    rise_.clear();
    // A node that holds an alert level reports its dry-mode measurements too: the
    // all clear is judged on levels measured recently, and the gateway can reach
    // this node only after a packet of its own. On a dry day nothing is sent.
    if (alertLevel_ != AlertLevel::None) readingDue_ = true;
  }
}

// Takes in what the receiver has heard. The codec is the only way in: a frame that
// is forged, foreign or replayed is dropped here.
void WadiNode::handleReceivedFrames(uint64_t nowMs) {
  if (!receiverOn_) return;
  uint8_t frame[kMaxPacketSize];
  size_t length;
  while ((length = radio_.receive(frame, sizeof(frame))) > 0) {
    Packet packet;
    if (!codec_.decode(frame, length, packet)) continue;
    if (packet.kind == PacketKind::AllClear) {
      handleAllClear(packet, nowMs);
    } else {
      upstream_.onPacket(packet, nowMs);
    }
  }
}

// An all clear ends the event at the wadi node it names. Like the siren node, this
// node takes it from any node of the site its codec accepts: with one key for the
// whole site, the sender's identifier proves nothing the authentication code does not.
void WadiNode::handleAllClear(const Packet& allClear, uint64_t nowMs) {
  const NodeId cleared = allClear.allClear.wadiNode;
  // The event at the upstream node is over: a Warning heard from it must not turn a
  // later Warning here into Evacuate.
  if (cleared != kNoNode && cleared == upstream_.node()) upstream_.forget();
  if (cleared != config_.id) return;

  // An all clear is given for a wadi and names each of its wadi nodes in turn. The
  // upstream node is on the same wadi, so its event has ended with this one.
  upstream_.forget();
  if (alertLevel_ == AlertLevel::None) return;
  alertLevel_ = AlertLevel::None;
  // The alert is over: the transmissions it still had to come are not made, and so
  // nothing holds back the reading below.
  alertRepeater_.cancel();
  // A reading at once, carrying no alert level: it tells the gateway that this node
  // has heard, so the gateway stops answering it. It goes out before the alert
  // level is looked at again, so an alert raised next is plainly a new one.
  sendReading(nowMs);
}

// Whether this node has anything an all clear would end. Only then is the answer
// window worth its energy: on a dry day the receiver stays off.
bool WadiNode::holdsWhatAnAllClearEnds(uint64_t nowMs) const {
  return alertLevel_ != AlertLevel::None || upstream_.alertLevel(nowMs) != AlertLevel::None;
}

// The alert engine decides; this keeps the highest alert level it has called for.
// Alert levels only escalate automatically; lowering one takes an all clear.
void WadiNode::raiseAlertLevel(uint64_t nowMs) {
  // Until the first measurement the level is that of a dry bed.
  const float sustainedRateOfRiseCmPerMin =
      rise_.sustainedRateCmPerMin(config_.alertThresholds.warningRateOfRiseSustainMs);
  const AlertInputs inputs = {levelCm_, sustainedRateOfRiseCmPerMin, rain_.rainMm(nowMs),
                              upstream_.alertLevel(nowMs), alertLevel_};
  const AlertLevel evaluated = evaluateAlertLevel(inputs, config_.alertThresholds);
  if (evaluated <= alertLevel_) return;
  alertLevel_ = evaluated;
  announceAlert(nowMs, /* refresh */ false);
}

// Watch is announced once. From Warning up the alert is sent again and again, so that
// a receiver that missed it, and the node downstream, still hear it.
bool WadiNode::hasAlertToRefresh() const { return alertLevel_ >= AlertLevel::Warning; }

void WadiNode::announceAlert(uint64_t nowMs, bool refresh) {
  sendAlert(refresh);
  nextAlertRefreshAtMs_ = nowMs + config_.alertRefreshIntervalMs;
}

// Each alert refresh is a new packet with its own counter: receivers accept it,
// and it says that it is one. It is an alert like any other, so it goes out several
// times and takes over from whatever transmissions an earlier alert still had to come.
void WadiNode::refreshAlert(uint64_t nowMs) {
  if (hasAlertToRefresh() && nowMs >= nextAlertRefreshAtMs_) {
    announceAlert(nowMs, /* refresh */ true);
  }
}

// The upstream link says when the receiver is worth its energy for hearing the
// upstream node; the answer window keeps it on after a transmission of this node's.
void WadiNode::switchReceiver(uint64_t nowMs) {
  const bool wanted = upstream_.wantsReceiver(nowMs, mode_.mode(), alertLevel_) ||
                      nowMs < answerWindowEndsAtMs_;
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

void WadiNode::sendAlert(bool refresh) {
  Packet packet = Packet();
  packet.kind = PacketKind::Alert;
  packet.alert.alertLevel = alertLevel_;
  packet.alert.refresh = refresh;
  packet.alert.levelMm = levelMm(levelCm_);
  packet.alert.levelRepeated = !levelMeasured_;
  packet.alert.rateOfRiseMmPerMin = rateMmPerMin(rise_.rateCmPerMin());
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

// What the gateway forwards to the dashboard for its curves. A reading goes out
// once: it is not life-safety traffic.
void WadiNode::sendReading(uint64_t nowMs) {
  readingDue_ = false;
  Packet packet = Packet();
  packet.kind = PacketKind::Reading;
  packet.reading.levelMm = levelMm(levelCm_);
  packet.reading.levelRepeated = !levelMeasured_;
  packet.reading.rateOfRiseMmPerMin = rateMmPerMin(rise_.rateCmPerMin());
  packet.reading.rainfallTenthsMm = rainTenthsMm(rain_.rainMm(nowMs));
  packet.reading.alertLevel = alertLevel_;
  packet.reading.stormMode = mode_.mode() == OperatingMode::Storm;
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
  packet.heartbeat.lastLevelRepeated = hasLevel_ && !levelMeasured_;
  // Each heartbeat reports the readings discarded since the one before.
  packet.heartbeat.discardedReadings = discardedReadings_;
  discardedReadings_ = 0;
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

// The only place this node touches the radio's transmitter. The gateway answers a
// packet from a node it has an all clear for, so the answer window opens here.
void WadiNode::transmit(const uint8_t* frame, size_t length) {
  if (length == 0) return;
  radio_.send(frame, length);
  const uint64_t nowMs = clock_.nowMs();
  if (holdsWhatAnAllClearEnds(nowMs)) answerWindowEndsAtMs_ = nowMs + config_.answerWindowMs;
}

void WadiNode::sleepUntilNextDue(uint64_t nowMs) {
  uint64_t nextDueAtMs = nextMeasurementAtMs_;
  if (alertRepeater_.pending()) {
    // A heartbeat that falls due meanwhile waits for the alert's last transmission.
    if (alertRepeater_.dueAtMs() < nextDueAtMs) nextDueAtMs = alertRepeater_.dueAtMs();
  } else if (nextHeartbeatAtMs_ < nextDueAtMs) {
    nextDueAtMs = nextHeartbeatAtMs_;
  }
  if (hasAlertToRefresh() && nextAlertRefreshAtMs_ < nextDueAtMs) {
    nextDueAtMs = nextAlertRefreshAtMs_;
  }
  // The end of an open receive window is due work too: the receiver has to be switched off.
  const uint64_t windowEndsAtMs = upstream_.windowEndsAtMs();
  if (receiverOn_ && windowEndsAtMs > nowMs && windowEndsAtMs < nextDueAtMs) {
    nextDueAtMs = windowEndsAtMs;
  }
  if (receiverOn_ && answerWindowEndsAtMs_ > nowMs && answerWindowEndsAtMs_ < nextDueAtMs) {
    nextDueAtMs = answerWindowEndsAtMs_;
  }
  if (nextDueAtMs > nowMs) sleep_.requestSleepMs(nextDueAtMs - nowMs);
}

}  // namespace wadiwatch
