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
//
// The all clear comes down the same uplink as a command of the backend, for one
// wadi. The gateway then ends the event at that wadi, queues the closing SMS for
// everyone who received an alert for it, and transmits a signed all clear packet
// naming each wadi node of the wadi, which is also forwarded to the backend like
// any packet it hears. How a wadi node that sleeps is made to hear it is described
// at WadiNodeAllClear below.
#pragma once

#include <cstddef>
#include <cstdint>

#include "wadiwatch/alert_repeater.h"
#include "wadiwatch/downlink_command.h"
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
          SlotStorage& messageStorage,
          RandomSource& random);

  // Call repeatedly. Handles every frame and every command received since the last
  // call, then makes at most one radio transmission and hands at most one SMS to
  // the modem and at most one message to the uplink, so that a slow modem or
  // uplink never keeps the gateway away from the radio for long.
  void tick();

 private:
  // One recipient's part in the events at one wadi.
  struct Notice {
    // The alert level this recipient is to be told of. None while the event
    // does not concern them.
    AlertLevel due = AlertLevel::None;
    // The alert level of the last SMS the modem accepted for this recipient in
    // the event in progress. An SMS is waiting to be sent whenever `due` is above it.
    AlertLevel sent = AlertLevel::None;
    // The recipient received an alert for an event that has ended and has not yet
    // been sent its closing SMS.
    bool closingDue = false;
  };

  // An event at one wadi. It starts when the wadi goes above no alert and
  // lasts until the all clear.
  struct Event {
    // Highest alert level heard for the wadi. None: no event in progress.
    AlertLevel alertLevel = AlertLevel::None;
    // One per slot of the recipient list.
    Notice notices[kMaxRecipients];
  };

  // The all clear for one wadi node.
  //
  // A siren node listens all the time; a wadi node listens only during the answer
  // window after a packet of its own. So the gateway does not transmit the moment
  // the command arrives: it holds the all clear for a wadi node until it next hears
  // that node, and transmits then (a node at Warning or above sends its alert again
  // within a minute, so node and siren node hear the same packet and the siren is
  // not sounded again by a node that missed it). After the hold time it transmits
  // anyway.
  //
  // A wadi node that showed an alert level before the command is awaited: until its
  // reading shows no alert level, an alert refresh from it is the old alert, not a
  // new event. The gateway answers it with the all clear again and starts no event
  // for it. The wait ends after a number of answers or a time, whichever comes
  // first; from then on the node's refresh is a new event, as it is for a gateway
  // that restarted.
  //
  // An alert that raises a node's alert level is never the old alert. It always
  // starts or escalates an event, it is never answered with an all clear, and an
  // all clear still held for that node is dropped. The same goes for an alert
  // refresh at a higher alert level than the all clear ended: the gateway missed
  // the alert that raised it.
  struct WadiNodeAllClear {
    // The alert level the node last showed, in an alert or a reading.
    AlertLevel shown = AlertLevel::None;
    // The alert level the all clear ended at the node: what the node showed when the
    // command came or, for a gateway that had not heard of it, first showed after.
    AlertLevel ended = AlertLevel::None;
    // The command the all clear belongs to.
    uint32_t command = 0;
    // An all clear naming the node is to be transmitted, at dueAtMs at the latest.
    bool due = false;
    uint64_t dueAtMs = 0;
    bool awaited = false;
    uint64_t awaitedUntilMs = 0;
    uint8_t answersLeft = 0;
  };

  void handleReceivedFrames(uint64_t nowMs);
  void keepForBackend(const Packet& packet, uint64_t nowMs);
  void forwardOneStoredMessage(uint64_t nowMs);
  bool timeReceived(const StoredMessage& message, uint64_t nowMs, uint32_t& unixSeconds);
  uint32_t secondsSinceStart(uint64_t nowMs) const;
  void handleAlert(NodeId sender, AlertLevel alertLevel);
  bool findWadiOf(NodeId wadiNode, size_t& wadiIndex) const;
  bool findWadiNode(NodeId wadiNode, size_t& wadiIndex, size_t& nodeIndex) const;
  void sendOneWaitingSms(uint64_t nowMs);
  bool sendAlertSms(size_t wadiIndex, size_t recipientIndex, AlertLevel alertLevel);
  bool sendClosingSms(size_t wadiIndex, size_t recipientIndex);

  void handleCommands(uint64_t nowMs);
  void handleAllClearCommand(const AllClearCommand& command, uint64_t nowMs);
  void endEvent(size_t wadiIndex);
  bool handleAwaitedPacket(WadiNodeAllClear& allClear, const Packet& packet, uint64_t nowMs);
  void transmitOneAllClear(uint64_t nowMs);

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

  // One per wadi node of each wadi, in the order of the configuration.
  WadiNodeAllClear allClears_[kMaxWadis][kMaxWadiNodesPerWadi];
  // The all clear packet being transmitted its several times. Until its last copy
  // has gone out the gateway sends no other packet: receivers would refuse the
  // copies still to come as older than what they last accepted.
  AlertRepeater allClearRepeater_;
  // The last command acted on, by its number and the time it was issued, so that
  // one delivered twice is acted on once.
  uint32_t lastCommand_ = 0;
  uint32_t lastCommandIssuedAt_ = 0;
};

}  // namespace wadiwatch
