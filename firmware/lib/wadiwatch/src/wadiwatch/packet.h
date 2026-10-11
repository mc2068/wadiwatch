// Radio packets shared by every device: the wire format, authentication and counter freshness.
#pragma once

#include <cstddef>
#include <cstdint>

#include "wadiwatch/ports.h"

namespace wadiwatch {

using NodeId = uint16_t;
// No node has this identifier: it stands for "none" wherever a node is optional.
constexpr NodeId kNoNode = 0;

enum class AlertLevel : uint8_t { None = 0, Watch = 1, Warning = 2, Evacuate = 3 };

enum class PacketKind : uint8_t {
  Reading = 1,
  Alert = 2,
  Acknowledgement = 3,
  Heartbeat = 4,
  AllClear = 5,
  Test = 6,
};

// Payload fields hold exactly what travels on the air, in the units named, so nothing is
// rounded between sender and receiver.

// A wadi node's measurement.
struct ReadingPayload {
  uint16_t levelMm;
  int16_t rateOfRiseMmPerMin;
  // Rain over the site's rainfall window, in tenths of a millimetre.
  uint16_t rainfallTenthsMm;
  AlertLevel alertLevel;
  bool stormMode;
  // The node's latest measurement gave no usable level: levelMm repeats the last
  // level it did measure (zero if it has measured none) and says nothing about the
  // wadi now.
  bool levelRepeated;
};

// A wadi node's alert level, with the measurement behind it.
struct AlertPayload {
  AlertLevel alertLevel;
  uint16_t levelMm;
  int16_t rateOfRiseMmPerMin;
  // False for the alert that raises the node's alert level; true for an alert
  // refresh, which a node at Warning or above sends every minute to repeat the level
  // it already announced. The gateway needs the difference after an all clear: a
  // refresh comes from a node that has not heard it, a raise is a new event.
  bool refresh;
  // As in a reading: levelMm is not from the node's latest measurement, which gave
  // no usable level.
  bool levelRepeated;
};

// A siren node's answer to an alert it accepted: which alert, and the level it is acting on.
struct AcknowledgementPayload {
  NodeId alertSender;
  uint32_t alertCounter;
  AlertLevel alertLevel;
};

// Proof of life from any node.
struct HeartbeatPayload {
  uint16_t batteryMillivolts;
  int16_t signalStrengthDbm;
  // Wadi nodes only: the last measured level. Meaningful only when hasLastLevel is set.
  uint16_t lastLevelMm;
  // Wadi nodes only: level-sensor pings discarded since the previous heartbeat, for
  // giving no echo or a physically impossible distance. Stops counting at 255.
  uint8_t discardedReadings;
  bool tamper;
  // False until the node has taken a level measurement (and always false for a siren node).
  bool hasLastLevel;
  // Siren nodes only: fell silent because no alert refresh was heard.
  bool alertRefreshLost;
  // Wadi nodes only, as in a reading: lastLevelMm is not from the node's latest
  // measurement, which gave no usable level.
  bool lastLevelRepeated;
};

// The end of the event at one wadi node, confirmed by an authorised officer.
struct AllClearPayload {
  NodeId wadiNode;
  // The backend's number for the all clear command the gateway is relaying. Every
  // all clear packet sent for one command carries it, so the backend can tell which
  // officer's decision a packet belongs to. Receivers do not act on it.
  uint32_t command;
};

// A test packet has no payload: its sender is the siren node under test.
struct Packet {
  PacketKind kind;
  // Set by PacketCodec::encode.
  NodeId sender;
  uint32_t counter;
  // Only the member matching `kind` is meaningful.
  union {
    ReadingPayload reading;
    AlertPayload alert;
    AcknowledgementPayload acknowledgement;
    HeartbeatPayload heartbeat;
    AllClearPayload allClear;
  };
};

// Every packet is this long on the air, authentication code included.
constexpr size_t kPacketSize = 24;
// Size to give a receive buffer.
constexpr size_t kMaxPacketSize = 32;

// A packet without its authentication code is this long: the first bytes of its frame.
constexpr size_t kPacketContentSize = 16;

// For a device's own storage: the content of a packet it has already accepted (or sent),
// in the layout of a frame without the authentication code. Returns false, and writes
// nothing useful, for a packet that makes no sense.
bool writePacketContent(const Packet& packet, uint8_t* content);
// Reads back what writePacketContent wrote. Never use this on bytes from the radio:
// nothing here checks who made them. PacketCodec::decode is the only way in from the air.
bool readPacketContent(const uint8_t* content, Packet& packet);

constexpr size_t kSiteKeySize = 32;
constexpr size_t kMaxSiteNodes = 8;

// The part of the site configuration that packets are checked against.
struct SiteAuth {
  // Shared by every node of the site and by nobody else.
  uint8_t key[kSiteKeySize];
  // Every node of the site: wadi nodes, siren nodes and the gateway.
  NodeId nodes[kMaxSiteNodes];
  uint8_t nodeCount;
};

// One device's end of the radio link. Everything a device sends or hears goes through here.
class PacketCodec {
 public:
  // Counters are kept in `storage`, which must outlive deep sleep and restarts.
  PacketCodec(const SiteAuth& auth, NodeId self, PersistentStorage& storage);

  // Stamps the packet with this node's identifier and its next message counter, then
  // signs and encodes it. Returns the number of bytes written, or 0 if nothing was
  // encoded (buffer too small, invalid packet, counter exhausted).
  // To repeat a transmission send the same bytes again: encoding again makes a new packet.
  size_t encode(Packet& packet, uint8_t* buffer, size_t capacity);

  // Returns true, and fills in the packet, only for a frame that is well formed, carries
  // a valid authentication code, comes from another node of the site, and has a counter
  // greater than the last one accepted from that node. A frame is accepted at most once.
  bool decode(const uint8_t* data, size_t length, Packet& packet);

 private:
  int indexOfNode(NodeId node) const;

  SiteAuth auth_;
  NodeId self_;
  PersistentStorage& storage_;
  // Counters are read from storage on first use, then kept here and written through.
  uint32_t sendCounter_;
  bool sendCounterLoaded_;
  uint32_t lastAccepted_[kMaxSiteNodes];
  bool lastAcceptedLoaded_[kMaxSiteNodes];
};

}  // namespace wadiwatch
