#include "wadiwatch/gateway.h"

#include "wadiwatch/sms_templates.h"

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

Gateway::Gateway(const GatewayConfig& config, const RecipientList& recipients, Clock& clock,
                 Radio& radio, GsmModem& modem)
    : config_(config), recipients_(recipients), clock_(clock), radio_(radio), modem_(modem) {
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
  handleReceivedFrames();
  sendOneWaitingSms(clock_.nowMs());
}

void Gateway::handleReceivedFrames() {
  uint8_t frame[kMaxPacketSize];
  size_t length;
  while ((length = radio_.receive(frame, sizeof(frame))) > 0) {
    Packet packet;
    if (!decodePacket(frame, length, packet)) continue;
    if (packet.kind == PacketKind::Alert) handleAlert(packet.sender, packet.alertLevel);
  }
}

void Gateway::handleAlert(NodeId sender, AlertLevel alertLevel) {
  size_t wadiIndex;
  // Not one of this site's wadi nodes: not ours to act on.
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
