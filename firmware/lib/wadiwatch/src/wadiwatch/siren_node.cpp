#include "wadiwatch/siren_node.h"

namespace wadiwatch {

namespace {

// Watch is for officials: only Warning and Evacuate reach the siren and the strobe.
bool sounds(AlertLevel alertLevel) { return alertLevel >= AlertLevel::Warning; }

}  // namespace

SirenNode::SirenNode(const SirenNodeConfig& config,
                     const SiteAuth& auth,
                     Clock& clock,
                     Radio& radio,
                     SirenOutput& output,
                     PersistentStorage& storage,
                     NodeHealth& health,
                     Button& button)
    : config_(config),
      clock_(clock),
      radio_(radio),
      output_(output),
      storage_(storage),
      health_(health),
      button_(button),
      codec_(auth, config.id, storage) {
  // Only the list of nodes is set up here. What was latched before a restart is
  // read on the first tick: the storage may not be ready when a core is built.
  for (size_t i = 0; i < auth.nodeCount && i < kMaxSiteNodes; ++i) {
    if (auth.nodes[i] == config.id) continue;
    const LatchedAlert nothingLatched = {auth.nodes[i], AlertLevel::None, 0, false};
    latches_[latchCount_++] = nothingLatched;
  }
}

void SirenNode::tick() {
  const uint64_t nowMs = clock_.nowMs();
  if (!started_) start(nowMs);

  handleReceivedFrames(nowMs);
  checkAlertRefresh(nowMs);
  handleButton(nowMs);
  driveOutput(nowMs);

  if (nowMs >= nextHeartbeatAtMs_) {
    sendHeartbeat();
    nextHeartbeatAtMs_ = nowMs + config_.heartbeatIntervalMs;
  }
}

// Picks up the alerts that were latched when the node last stopped. How long it was
// off is unknown, so each one gets a whole refresh timeout from now.
void SirenNode::start(uint64_t nowMs) {
  started_ = true;
  nextHeartbeatAtMs_ = nowMs + config_.heartbeatIntervalMs;
  for (size_t i = 0; i < latchCount_; ++i) {
    LatchedAlert& latch = latches_[i];
    uint32_t stored = 0;
    if (storage_.load(storageKey(StorageRecord::LatchedAlert, latch.wadiNode), stored) &&
        stored <= static_cast<uint32_t>(AlertLevel::Evacuate)) {
      latch.alertLevel = static_cast<AlertLevel>(stored);
    }
    latch.refreshedAtMs = nowMs;
  }
}

void SirenNode::handleReceivedFrames(uint64_t nowMs) {
  uint8_t frame[kMaxPacketSize];
  size_t length;
  while ((length = radio_.receive(frame, sizeof(frame))) > 0) {
    Packet packet;
    // The codec is the only way in: a frame that is forged, foreign or replayed is dropped
    // here, and so is every further transmission of an alert already accepted.
    if (!codec_.decode(frame, length, packet)) continue;
    if (packet.kind == PacketKind::Alert) {
      handleAlert(packet, nowMs);
    } else if (packet.kind == PacketKind::AllClear) {
      handleAllClear(packet);
    }
  }
}

void SirenNode::handleAlert(const Packet& alert, uint64_t nowMs) {
  LatchedAlert* latch = latchFor(alert.sender);
  if (latch == nullptr) return;

  // Any alert from the wadi node shows that it is alive and still in alert, so it
  // counts as a refresh whatever its level. A lower level does not quieten the
  // siren: alert levels come down only through an all clear.
  latch->refreshedAtMs = nowMs;
  latch->refreshLost = false;
  if (alert.alert.alertLevel > latch->alertLevel) {
    latch->alertLevel = alert.alert.alertLevel;
    storeLatch(*latch);
  }

  Packet acknowledgement = Packet();
  acknowledgement.kind = PacketKind::Acknowledgement;
  acknowledgement.acknowledgement.alertSender = alert.sender;
  acknowledgement.acknowledgement.alertCounter = alert.counter;
  acknowledgement.acknowledgement.alertLevel = latch->alertLevel;
  send(acknowledgement);
}

void SirenNode::handleAllClear(const Packet& allClear) {
  LatchedAlert* latch = latchFor(allClear.allClear.wadiNode);
  if (latch == nullptr || latch->alertLevel == AlertLevel::None) return;
  latch->alertLevel = AlertLevel::None;
  latch->refreshLost = false;
  storeLatch(*latch);
}

// Starts a test, or silences the one running. A test also ends by itself, and at
// once when an alert sounds. The button never touches an alert.
void SirenNode::handleButton(uint64_t nowMs) {
  const bool alertSounding = sounds(soundingAlertLevel());
  if (testRunning_ &&
      (alertSounding || nowMs - testStartedAtMs_ >= config_.testDurationMs)) {
    testRunning_ = false;
  }

  // The button is read on every tick, so that a press made during an alert is
  // forgotten instead of starting a test once the alert is over.
  if (!button_.wasPressed() || alertSounding) return;
  if (testRunning_) {
    testRunning_ = false;
    return;
  }
  testRunning_ = true;
  testStartedAtMs_ = nowMs;
  Packet test = Packet();
  test.kind = PacketKind::Test;
  send(test);
}

// A wadi node at Warning or above sends an alert refresh every minute. When a latched Warning or
// Evacuate has heard nothing for the whole timeout, the siren node stops sounding
// for it and says so in a heartbeat straight away. The alert stays latched: the
// next alert from that wadi node sounds the siren again. Watch has no alert refresh and
// is silent anyway, so it is left alone.
void SirenNode::checkAlertRefresh(uint64_t nowMs) {
  bool lostNow = false;
  for (size_t i = 0; i < latchCount_; ++i) {
    LatchedAlert& latch = latches_[i];
    if (!sounds(latch.alertLevel) || latch.refreshLost) continue;
    if (nowMs - latch.refreshedAtMs < config_.alertRefreshTimeoutMs) continue;
    latch.refreshLost = true;
    lostNow = true;
  }
  if (lostNow) sendHeartbeat();
}

void SirenNode::driveOutput(uint64_t nowMs) {
  const AlertLevel alertLevel = soundingAlertLevel();
  // The pattern starts when Warning starts to sound. A repeat or a refresh of the
  // alert in force does not start it again.
  if (alertLevel == AlertLevel::Warning && outputAlertLevel_ != AlertLevel::Warning) {
    intermittentStartedAtMs_ = nowMs;
  }
  outputAlertLevel_ = alertLevel;

  bool siren = false;
  bool strobe = false;
  if (alertLevel == AlertLevel::Evacuate) {
    siren = true;
    strobe = true;
  } else if (alertLevel == AlertLevel::Warning) {
    strobe = true;
    const uint64_t periodMs =
        static_cast<uint64_t>(config_.intermittentOnMs) + config_.intermittentOffMs;
    // The pattern starts with the siren sounding. A pattern with no duration
    // configured must not leave a Warning silent.
    siren = periodMs == 0 ||
            (nowMs - intermittentStartedAtMs_) % periodMs < config_.intermittentOnMs;
  } else if (testRunning_) {
    // A test exercises both outputs.
    siren = true;
    strobe = true;
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

void SirenNode::sendHeartbeat() {
  Packet packet = Packet();
  packet.kind = PacketKind::Heartbeat;
  packet.heartbeat.batteryMillivolts = health_.batteryMillivolts();
  packet.heartbeat.signalStrengthDbm = health_.signalStrengthDbm();
  packet.heartbeat.tamper = health_.tamperDetected();
  packet.heartbeat.alertRefreshLost = refreshLost();
  send(packet);
}

// Everything this node transmits leaves through here: the codec stamps the
// packet with the node's identifier and next counter, and signs it.
void SirenNode::send(Packet& packet) {
  uint8_t frame[kMaxPacketSize];
  const size_t length = codec_.encode(packet, frame, sizeof(frame));
  if (length > 0) radio_.send(frame, length);
}

SirenNode::LatchedAlert* SirenNode::latchFor(NodeId wadiNode) {
  for (size_t i = 0; i < latchCount_; ++i) {
    if (latches_[i].wadiNode == wadiNode) return &latches_[i];
  }
  return nullptr;
}

// Written whenever the latched alert level changes, so that it survives a restart.
void SirenNode::storeLatch(const LatchedAlert& latch) {
  storage_.store(storageKey(StorageRecord::LatchedAlert, latch.wadiNode),
                 static_cast<uint32_t>(latch.alertLevel));
}

// The highest alert level in force: latched, and not silenced for want of a refresh.
AlertLevel SirenNode::soundingAlertLevel() const {
  AlertLevel highest = AlertLevel::None;
  for (size_t i = 0; i < latchCount_; ++i) {
    const LatchedAlert& latch = latches_[i];
    if (!latch.refreshLost && latch.alertLevel > highest) highest = latch.alertLevel;
  }
  return highest;
}

bool SirenNode::refreshLost() const {
  for (size_t i = 0; i < latchCount_; ++i) {
    if (latches_[i].refreshLost) return true;
  }
  return false;
}

}  // namespace wadiwatch
