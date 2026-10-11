#include "wadiwatch/gateway.h"

#include "wadiwatch/sms_templates.h"
#include "wadiwatch/uplink_message.h"

namespace wadiwatch {

namespace {

bool isRegistered(const Recipient& recipient) { return recipient.phoneNumber[0] != '\0'; }

// Whether a recipient is to be told that `wadi` has reached `alertLevel`.
bool concerns(AlertLevel alertLevel, WadiId wadi, const Recipient& recipient) {
  switch (alertLevel) {
    case AlertLevel::Evacuate:
      return true;
    case AlertLevel::Warning:
      return recipient.group == RecipientGroup::Official || recipient.nearestWadi == wadi;
    case AlertLevel::Watch:
      return recipient.group == RecipientGroup::Official;
    case AlertLevel::None:
      break;
  }
  return false;
}

const char* nameIn(const WadiConfig& wadi, Language language) {
  return language == Language::Arabic ? wadi.nameArabic : wadi.nameFrench;
}

}  // namespace

Gateway::Gateway(const GatewayConfig& config,
                 const SiteAuth& auth,
                 const CommandAuth& commandAuth,
                 const RecipientList& recipients,
                 Clock& clock,
                 Radio& radio,
                 GsmModem& modem,
                 PersistentStorage& storage,
                 WallClock& wallClock,
                 Uplink& uplink,
                 SlotStorage& messageStorage,
                 RandomSource& random)
    : config_(config),
      commandAuth_(commandAuth),
      recipients_(recipients),
      clock_(clock),
      radio_(radio),
      modem_(modem),
      codec_(auth, config.id, storage),
      wallClock_(wallClock),
      uplink_(uplink),
      messages_(messageStorage),
      allClearRepeater_(config.allClearRepeat, random) {
  // A configuration that claims more than there is room for must not send the
  // gateway reading past its tables.
  if (config_.wadiCount > kMaxWadis) config_.wadiCount = static_cast<uint8_t>(kMaxWadis);
  for (size_t w = 0; w < config_.wadiCount; ++w) {
    WadiConfig& wadi = config_.wadis[w];
    if (wadi.wadiNodeCount > kMaxWadiNodesPerWadi) {
      wadi.wadiNodeCount = static_cast<uint8_t>(kMaxWadiNodesPerWadi);
    }
  }
}

void Gateway::tick() {
  const uint64_t nowMs = clock_.nowMs();
  if (!started_) {
    started_ = true;
    startedAtMs_ = nowMs;
  }
  handleReceivedFrames(nowMs);
  handleCommands(nowMs);
  transmitOneAllClear(nowMs);
  sendOneWaitingSms(nowMs);
  forwardOneStoredMessage(nowMs);
}

void Gateway::handleReceivedFrames(uint64_t nowMs) {
  uint8_t frame[kMaxPacketSize];
  size_t length;
  while ((length = radio_.receive(frame, sizeof(frame))) > 0) {
    Packet packet;
    // The codec is the only way in: a frame that is forged, foreign or replayed is dropped
    // here, exactly as the siren node drops it. No SMS is ever sent for one, and the
    // backend never hears of it.
    if (!codec_.decode(frame, length, packet)) continue;
    // Every accepted packet goes to the backend, whatever its kind and whether or not
    // the gateway acts on it itself.
    keepForBackend(packet, nowMs);
    size_t wadiIndex;
    size_t nodeIndex;
    // What a wadi node sends after an all clear it has not heard is the alert that
    // all clear ended: it gets the all clear again, and starts no event.
    if (findWadiNode(packet.sender, wadiIndex, nodeIndex) &&
        handleAwaitedPacket(allClears_[wadiIndex][nodeIndex], packet, nowMs)) {
      continue;
    }
    if (packet.kind == PacketKind::Alert) handleAlert(packet.sender, packet.alert.alertLevel);
  }
}

// Puts the packet in the message store with the time it was received. It is forwarded
// from there, in its turn. A packet the gateway sends itself (the all clear) goes
// through here too.
void Gateway::keepForBackend(const Packet& packet, uint64_t nowMs) {
  StoredMessage message = StoredMessage();
  message.packet = packet;
  // Without the time of day the packet is dated by how long the gateway has been
  // running, and given its place in the day when the time becomes known.
  message.datedSinceStart = !wallClock_.nowUnixSeconds(message.receivedAt);
  if (message.datedSinceStart) message.receivedAt = secondsSinceStart(nowMs);
  messages_.add(message);
}

// Offers the uplink the message stored longest. Only when the uplink has taken it is
// it let go, so messages leave in the order their packets arrived, each exactly once
// as far as the gateway can tell.
void Gateway::forwardOneStoredMessage(uint64_t nowMs) {
  if (nowMs < nextUplinkAttemptAtMs_) return;

  StoredMessage message;
  if (!messages_.oldest(message)) return;
  uint32_t receivedAt;
  // Not datable yet. Nothing behind it may overtake it, so everything waits.
  if (!timeReceived(message, nowMs, receivedAt)) return;

  char text[kMaxUplinkMessageLength + 1];
  const size_t length =
      writeUplinkMessage(message.packet, receivedAt, config_, text, sizeof(text));
  // length is 0 only for a packet the contract has no message for. It must not hold
  // up the rest.
  if (length == 0 || uplink_.publish(text, length)) {
    messages_.removeOldest();
  } else {
    // The message stays stored. Leave the uplink alone for a while.
    nextUplinkAttemptAtMs_ = nowMs + config_.uplinkRetryIntervalMs;
  }
}

// The time of day the gateway received a stored packet. Returns false while that
// cannot be told yet.
bool Gateway::timeReceived(const StoredMessage& message, uint64_t nowMs, uint32_t& unixSeconds) {
  if (!message.datedSinceStart) {
    unixSeconds = message.receivedAt;
    return true;
  }
  if (!startTimeKnown_) {
    uint32_t nowUnixSeconds;
    if (!wallClock_.nowUnixSeconds(nowUnixSeconds)) return false;
    // Worked out once, so that a message offered again carries the same time.
    startedAtUnixSeconds_ = nowUnixSeconds - secondsSinceStart(nowMs);
    startTimeKnown_ = true;
  }
  // A packet stored before a restart was counted from a start that is gone. The
  // restart is the latest it can have been received, and that is what it is given.
  unixSeconds = message.fromEarlierRun ? startedAtUnixSeconds_
                                       : startedAtUnixSeconds_ + message.receivedAt;
  return true;
}

uint32_t Gateway::secondsSinceStart(uint64_t nowMs) const {
  return static_cast<uint32_t>((nowMs - startedAtMs_) / 1000);
}

void Gateway::handleAlert(NodeId sender, AlertLevel alertLevel) {
  size_t wadiIndex;
  // A node of the site, but not a wadi node mounted over one of its wadis: not an alert
  // to act on.
  if (!findWadiOf(sender, wadiIndex)) return;

  Event& event = events_[wadiIndex];
  // A repeat of the alert in force, or a lower alert level, is no change.
  if (alertLevel <= event.alertLevel) return;
  event.alertLevel = alertLevel;

  const WadiId wadi = config_.wadis[wadiIndex].id;
  for (size_t r = 0; r < kMaxRecipients; ++r) {
    const Recipient& recipient = recipients_.recipients[r];
    if (!isRegistered(recipient)) continue;
    // An SMS still waiting from a lower alert level is overtaken here: the
    // recipient will be told what is true now, once.
    if (concerns(alertLevel, wadi, recipient)) event.notices[r].due = alertLevel;
  }
}

bool Gateway::findWadiOf(NodeId wadiNode, size_t& wadiIndex) const {
  size_t nodeIndex;
  return findWadiNode(wadiNode, wadiIndex, nodeIndex);
}

bool Gateway::findWadiNode(NodeId wadiNode, size_t& wadiIndex, size_t& nodeIndex) const {
  for (size_t w = 0; w < config_.wadiCount; ++w) {
    const WadiConfig& wadi = config_.wadis[w];
    for (size_t n = 0; n < wadi.wadiNodeCount; ++n) {
      if (wadi.wadiNodes[n] == wadiNode) {
        wadiIndex = w;
        nodeIndex = n;
        return true;
      }
    }
  }
  return false;
}

// Takes in the commands the backend has sent down. One that is malformed, or does
// not carry the authentication code of the command key, is dropped whole: the
// gateway never acts on part of a command, nor on a command from anyone but the
// backend.
void Gateway::handleCommands(uint64_t nowMs) {
  char text[kMaxDownlinkCommandLength + 1];
  size_t length;
  while ((length = uplink_.receive(text, sizeof(text))) > 0) {
    AllClearCommand command;
    if (readAllClearCommand(text, length, commandAuth_, command)) {
      handleAllClearCommand(command, nowMs);
    }
  }
}

// An officer's all clear for one wadi. It covers every wadi node of that wadi.
void Gateway::handleAllClearCommand(const AllClearCommand& command, uint64_t nowMs) {
  // A command can wait at the broker through an uplink outage and arrive when the
  // wadi is in flood again. One that is not fresh is not acted on, and neither is
  // one whose age the gateway cannot tell: the sirens then go on sounding, which
  // the officer sees and can answer with a new all clear.
  uint32_t nowUnixSeconds;
  if (!wallClock_.nowUnixSeconds(nowUnixSeconds)) return;
  const uint64_t issuedAt = command.issuedAt;
  const uint64_t maxAgeS = config_.commandMaxAgeS;
  if (issuedAt + maxAgeS < nowUnixSeconds) return;
  if (issuedAt > nowUnixSeconds + maxAgeS) return;
  // The uplink delivers at least once: the same command may come twice. A backend
  // whose database was started afresh numbers its commands from one again, so the
  // number alone does not tell a command from the one before.
  if (command.command == lastCommand_ && command.issuedAt == lastCommandIssuedAt_) return;

  size_t wadiIndex = 0;
  while (wadiIndex < config_.wadiCount && config_.wadis[wadiIndex].id != command.wadi) ++wadiIndex;
  if (wadiIndex == config_.wadiCount) return;
  lastCommand_ = command.command;
  lastCommandIssuedAt_ = command.issuedAt;

  endEvent(wadiIndex);
  const WadiConfig& wadi = config_.wadis[wadiIndex];
  for (size_t n = 0; n < wadi.wadiNodeCount; ++n) {
    WadiNodeAllClear& allClear = allClears_[wadiIndex][n];
    allClear.command = command.command;
    allClear.due = true;
    allClear.dueAtMs = nowMs + config_.allClearHoldMs;
    allClear.ended = allClear.shown;
    allClear.awaited = allClear.shown != AlertLevel::None && config_.allClearMaxAnswers > 0;
    allClear.awaitedUntilMs = nowMs + config_.allClearAnswerPeriodMs;
    allClear.answersLeft = config_.allClearMaxAnswers;
  }
}

// The event at a wadi is over. Everyone who received an alert for it is owed the
// closing SMS; an alert SMS still waiting is not sent any more, and whoever was
// never told of the event is not told of its end. The next alert starts a new event.
void Gateway::endEvent(size_t wadiIndex) {
  Event& event = events_[wadiIndex];
  event.alertLevel = AlertLevel::None;
  for (size_t r = 0; r < kMaxRecipients; ++r) {
    Notice& notice = event.notices[r];
    const bool closingDue = notice.closingDue || notice.sent != AlertLevel::None;
    notice = Notice();
    notice.closingDue = closingDue;
  }
}

// A packet from a wadi node, looked at for what it means for that node's all clear.
// Returns true when the packet belongs to the event the all clear ended: the caller
// then does not act on it as an alert.
bool Gateway::handleAwaitedPacket(WadiNodeAllClear& allClear, const Packet& packet, uint64_t nowMs) {
  if (packet.kind == PacketKind::Alert) allClear.shown = packet.alert.alertLevel;
  if (packet.kind == PacketKind::Reading) allClear.shown = packet.reading.alertLevel;

  if (allClear.awaited && nowMs >= allClear.awaitedUntilMs) allClear.awaited = false;

  // An alert that raises the node's alert level is news whatever came before: the
  // node heard the all clear and the water has come back, or it restarted into a
  // flood, or it never heard and the flood has grown. It is never answered with an
  // all clear, and an all clear still held for the node is not transmitted. Only an
  // alert refresh can be the old alert, and only at an alert level the all clear
  // ended: a refresh above that level comes from a node whose raise was not heard.
  if (packet.kind == PacketKind::Alert) {
    const bool raiseNotHeard = allClear.awaited && packet.alert.alertLevel > allClear.ended;
    if (!packet.alert.refresh || raiseNotHeard) {
      allClear.awaited = false;
      allClear.due = false;
      return false;
    }
  }

  // An all clear is still held for the node and the node shows an alert level: one it
  // held when the command came, which a gateway that restarted had not heard of.
  if (allClear.due && !allClear.awaited && allClear.shown != AlertLevel::None &&
      config_.allClearMaxAnswers > 0) {
    allClear.ended = allClear.shown;
    allClear.awaited = true;
    allClear.awaitedUntilMs = nowMs + config_.allClearAnswerPeriodMs;
    allClear.answersLeft = config_.allClearMaxAnswers;
  }

  if (!allClear.awaited) {
    // The node has just transmitted, so it is listening: the moment for an all
    // clear held for it.
    if (allClear.due) allClear.dueAtMs = nowMs;
    return false;
  }

  if (packet.kind == PacketKind::Reading && packet.reading.alertLevel == AlertLevel::None) {
    // The node holds no alert level any more: it has heard.
    allClear.awaited = false;
    if (allClear.due) allClear.dueAtMs = nowMs;
    return false;
  }

  // A node at Warning or above asks with its alert refresh every minute; its
  // readings are left unanswered, to keep the gateway off the air. A node at Watch
  // has no alert refresh, so its readings ask (it sends one with every measurement
  // while it holds an alert level, in dry mode too), and any node's heartbeat.
  const bool asks =
      packet.kind == PacketKind::Alert || packet.kind == PacketKind::Heartbeat ||
      (packet.kind == PacketKind::Reading && packet.reading.alertLevel == AlertLevel::Watch);
  if (asks) {
    allClear.due = true;
    allClear.dueAtMs = nowMs;
    // After the last answer the node's next alert is a new event.
    if (--allClear.answersLeft == 0) allClear.awaited = false;
  }
  return true;
}

// Makes the next radio transmission, if one is due: a further copy of the all clear
// packet being repeated, or else the first copy of the all clear due longest.
void Gateway::transmitOneAllClear(uint64_t nowMs) {
  size_t length = 0;
  const uint8_t* copy = allClearRepeater_.takeDue(nowMs, length);
  if (copy != nullptr) {
    radio_.send(copy, length);
    return;
  }
  // Nothing else leaves the gateway between the first copy of a packet and the last.
  if (allClearRepeater_.pending()) return;

  WadiNodeAllClear* next = nullptr;
  NodeId wadiNode = kNoNode;
  for (size_t w = 0; w < config_.wadiCount; ++w) {
    const WadiConfig& wadi = config_.wadis[w];
    for (size_t n = 0; n < wadi.wadiNodeCount; ++n) {
      WadiNodeAllClear& allClear = allClears_[w][n];
      if (!allClear.due || nowMs < allClear.dueAtMs) continue;
      if (next != nullptr && next->dueAtMs <= allClear.dueAtMs) continue;
      next = &allClear;
      wadiNode = wadi.wadiNodes[n];
    }
  }
  if (next == nullptr) return;
  next->due = false;

  Packet packet = Packet();
  packet.kind = PacketKind::AllClear;
  packet.allClear.wadiNode = wadiNode;
  packet.allClear.command = next->command;
  uint8_t frame[kMaxPacketSize];
  // The codec stamps the packet with the gateway's identifier and next counter, and signs it.
  length = codec_.encode(packet, frame, sizeof(frame));
  if (length == 0) return;
  radio_.send(frame, length);
  // The backend learns of the all clear the way it learns of everything else.
  keepForBackend(packet, nowMs);
  allClearRepeater_.start(frame, length, nowMs);
}

void Gateway::sendOneWaitingSms(uint64_t nowMs) {
  if (nowMs < nextSmsAttemptAtMs_) return;

  // The slots are every recipient of every wadi's event, taken in turn.
  const size_t slotCount = static_cast<size_t>(config_.wadiCount) * kMaxRecipients;
  for (size_t step = 0; step < slotCount; ++step) {
    const size_t slot = (nextSlot_ + step) % slotCount;
    const size_t wadiIndex = slot / kMaxRecipients;
    const size_t recipientIndex = slot % kMaxRecipients;
    Notice& notice = events_[wadiIndex].notices[recipientIndex];
    const bool alertWaiting = notice.due > notice.sent;
    if (!alertWaiting && !notice.closingDue) continue;

    nextSlot_ = (slot + 1) % slotCount;
    bool sent;
    if (alertWaiting) {
      // A new event has overtaken a closing SMS still waiting from the last one:
      // the recipient is told what is true now, and of its end when it ends.
      sent = sendAlertSms(wadiIndex, recipientIndex, notice.due);
      if (sent) {
        notice.sent = notice.due;
        notice.closingDue = false;
      }
    } else {
      sent = sendClosingSms(wadiIndex, recipientIndex);
      if (sent) notice.closingDue = false;
    }
    // An SMS the modem refused stays waiting. Leave the modem alone for a while.
    if (!sent) nextSmsAttemptAtMs_ = nowMs + config_.smsRetryIntervalMs;
    return;
  }
}

bool Gateway::sendClosingSms(size_t wadiIndex, size_t recipientIndex) {
  const Recipient& recipient = recipients_.recipients[recipientIndex];
  char text[kMaxSmsTextLength + 1];
  renderClosingSms(recipient.language, nameIn(config_.wadis[wadiIndex], recipient.language), text,
                    sizeof(text));
  return modem_.sendSms(recipient.phoneNumber, text);
}

bool Gateway::sendAlertSms(size_t wadiIndex, size_t recipientIndex, AlertLevel alertLevel) {
  const Recipient& recipient = recipients_.recipients[recipientIndex];
  char text[kMaxSmsTextLength + 1];
  renderAlertSms(recipient.language, alertLevel, nameIn(config_.wadis[wadiIndex], recipient.language),
                 text, sizeof(text));
  return modem_.sendSms(recipient.phoneNumber, text);
}

}  // namespace wadiwatch
