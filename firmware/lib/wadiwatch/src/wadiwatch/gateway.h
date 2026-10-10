// Gateway core: hears the alerts the siren nodes hear, believes them by the same rules
// (the packet codec), and sends SMS to the registered recipients through its own GSM
// modem. Nothing in that needs the internet.
//
// Watch goes to officials only, Warning to officials and to residents
// registered near that wadi, Evacuate to every registered recipient. A
// recipient gets one SMS per change of alert level per event.
//
// It also forwards every packet it accepts to the backend over the uplink, as a
// message of docs/gateway-contract.md dated with the time the packet was received.
// Packets wait in the message store until the uplink has taken them, so an uplink
// outage or a restart delays messages and does not lose or reorder them. The SMS
// do not wait for the uplink, nor the uplink for the SMS.
#pragma once

#include <cstddef>
#include <cstdint>

#include "wadiwatch/message_store.h"
#include "wadiwatch/packet.h"
#include "wadiwatch/ports.h"
#include "wadiwatch/recipients.h"
#include "wadiwatch/site_config.h"

namespace wadiwatch {

class Gateway {
 public:
  Gateway(const GatewayConfig& config,
          const SiteAuth& auth,
          const RecipientList& recipients,
          Clock& clock,
          Radio& radio,
          GsmModem& modem,
          PersistentStorage& storage,
          WallClock& wallClock,
          Uplink& uplink,
          SlotStorage& messageStorage);

  // Call repeatedly. Handles every frame received since the last call, then
  // hands at most one SMS to the modem and at most one message to the uplink,
  // so that a slow modem or uplink never keeps the gateway away from the radio
  // for long.
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

  void handleReceivedFrames(uint64_t nowMs);
  void keepForBackend(const Packet& packet, uint64_t nowMs);
  void forwardOneStoredMessage(uint64_t nowMs);
  bool timeReceived(const StoredMessage& message, uint64_t nowMs, uint32_t& unixSeconds);
  uint32_t secondsSinceStart(uint64_t nowMs) const;
  void handleAlert(NodeId sender, AlertLevel alertLevel);
  bool findWadiOf(NodeId wadiNode, size_t& wadiIndex) const;
  void sendOneWaitingSms(uint64_t nowMs);
  bool sendAlertSms(size_t wadiIndex, size_t recipientIndex, AlertLevel alertLevel);

  GatewayConfig config_;
  RecipientList recipients_;
  Clock& clock_;
  Radio& radio_;
  GsmModem& modem_;
  PacketCodec codec_;
  // One per wadi of the configuration, in the same order.
  Event events_[kMaxWadis];
  // Where the search for the next waiting SMS resumes, so that every waiting
  // SMS gets its turn even if one of them can never be sent.
  size_t nextSlot_ = 0;
  uint64_t nextSmsAttemptAtMs_ = 0;

  WallClock& wallClock_;
  Uplink& uplink_;
  // Every accepted packet, until the uplink has taken its message.
  MessageStore messages_;
  bool started_ = false;
  uint64_t startedAtMs_ = 0;
  // The time of day at which this run of the gateway started, worked out the
  // first time the wall clock is known. It dates the packets received before that.
  bool startTimeKnown_ = false;
  uint32_t startedAtUnixSeconds_ = 0;
  uint64_t nextUplinkAttemptAtMs_ = 0;
};

}  // namespace wadiwatch
