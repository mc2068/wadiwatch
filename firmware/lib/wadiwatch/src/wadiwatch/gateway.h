// Gateway core: hears the alerts the siren nodes hear and sends SMS to the
// registered recipients through its own GSM modem. Nothing here needs the internet.
//
// Watch goes to officials only, Warning to officials and to residents
// registered near that wadi, Evacuate to every registered recipient. A
// recipient gets one SMS per change of alert level per event.
#pragma once

#include <cstddef>
#include <cstdint>

#include "wadiwatch/packet.h"
#include "wadiwatch/ports.h"
#include "wadiwatch/recipients.h"
#include "wadiwatch/site_config.h"

namespace wadiwatch {

class Gateway {
 public:
  Gateway(const GatewayConfig& config, const RecipientList& recipients, Clock& clock, Radio& radio,
          GsmModem& modem);

  // Call repeatedly. Handles every frame received since the last call, then
  // hands at most one SMS to the modem, so that a slow modem never keeps the
  // gateway away from the radio for long.
  void tick();

 private:
  // One recipient's part in one event.
  struct Notice {
    // The alert level this recipient is to be told of. None while the event
    // does not concern them.
    AlertLevel due = AlertLevel::None;
    // The alert level of the last SMS the modem accepted for this recipient.
    // An SMS is waiting to be sent whenever `due` is above it.
    AlertLevel sent = AlertLevel::None;
  };

  // An event at one wadi. It starts when the wadi goes above no alert and
  // lasts until the all clear, which the gateway does not handle yet.
  struct Event {
    // Highest alert level heard for the wadi. None: no event in progress.
    AlertLevel alertLevel = AlertLevel::None;
    // One per slot of the recipient list.
    Notice notices[kMaxRecipients];
  };

  void handleReceivedFrames();
  void handleAlert(NodeId sender, AlertLevel alertLevel);
  bool findWadiOf(NodeId wadiNode, size_t& wadiIndex) const;
  void sendOneWaitingSms(uint64_t nowMs);
  bool sendAlertSms(size_t wadiIndex, size_t recipientIndex, AlertLevel alertLevel);

  GatewayConfig config_;
  RecipientList recipients_;
  Clock& clock_;
  Radio& radio_;
  GsmModem& modem_;
  // One per wadi of the configuration, in the same order.
  Event events_[kMaxWadis];
  // Where the search for the next waiting SMS resumes, so that every waiting
  // SMS gets its turn even if one of them can never be sent.
  size_t nextSlot_ = 0;
  uint64_t nextSmsAttemptAtMs_ = 0;
};

}  // namespace wadiwatch
