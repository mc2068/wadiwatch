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
                 const RecipientList& recipients,
                 Clock& clock,
                 Radio& radio,
                 GsmModem& modem,
                 PersistentStorage& storage,
                 WallClock& wallClock,
                 Uplink& uplink,
                 SlotStorage& messageStorage)
    : config_(config),
      recipients_(recipients),
      clock_(clock),
      radio_(radio),
      modem_(modem),
      codec_(auth, config.id, storage),
      wallClock_(wallClock),
      uplink_(uplink),
      messages_(messageStorage) {
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
    if (packet.kind == PacketKind::Alert) handleAlert(packet.sender, packet.alert.alertLevel);
  }
}

// Puts the packet in the message store with the time it was received. It is forwarded
// from there, in its turn. A packet the gateway sends itself (the all clear) belongs
// here too, once the gateway sends one.
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
  for (size_t w = 0; w < config_.wadiCount; ++w) {
    const WadiConfig& wadi = config_.wadis[w];
    for (size_t n = 0; n < wadi.wadiNodeCount; ++n) {
      if (wadi.wadiNodes[n] == wadiNode) {
        wadiIndex = w;
        return true;
      }
    }
  }
  return false;
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
    if (notice.due <= notice.sent) continue;

    nextSlot_ = (slot + 1) % slotCount;
    if (sendAlertSms(wadiIndex, recipientIndex, notice.due)) {
      notice.sent = notice.due;
    } else {
      // The SMS stays waiting. Leave the modem alone for a while.
      nextSmsAttemptAtMs_ = nowMs + config_.smsRetryIntervalMs;
    }
    return;
  }
}

bool Gateway::sendAlertSms(size_t wadiIndex, size_t recipientIndex, AlertLevel alertLevel) {
  const Recipient& recipient = recipients_.recipients[recipientIndex];
  char text[kMaxSmsTextLength + 1];
  renderAlertSms(recipient.language, alertLevel, nameIn(config_.wadis[wadiIndex], recipient.language),
                 text, sizeof(text));
  return modem_.sendSms(recipient.phoneNumber, text);
}

}  // namespace wadiwatch
