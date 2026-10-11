#include "wadiwatch/packet.h"

#include <cstring>

#include "wadiwatch/sha256.h"

namespace wadiwatch {

namespace {

// Frame layout, multi-byte fields big-endian:
//
//   0      protocol version
//   1      packet kind
//   2-3    sender node identifier
//   4-7    message counter
//   8-15   payload, by kind (unused bytes are zero):
//            reading          level mm (2), rate of rise mm/min (2), rainfall 0.1 mm (2),
//                             alert level (1), flags (1): storm mode 0x01,
//                             level repeated 0x02
//            alert            alert level (1), level mm (2), rate of rise mm/min (2),
//                             flags (1): alert refresh 0x01, level repeated 0x02
//            acknowledgement  alert sender (2), alert counter (4), alert level (1)
//            heartbeat        battery mV (2), signal dBm (2), last level mm (2),
//                             discarded readings (1), flags (1): tamper 0x01,
//                             alert refresh lost 0x02, has last level 0x04,
//                             last level repeated 0x08
//            all clear        wadi node identifier (2), command (4)
//            test             nothing
//   16-23  authentication code: HMAC-SHA-256 of bytes 0-15 under the site key,
//          cut to its first 8 bytes
constexpr uint8_t kProtocolVersion = 2;
constexpr size_t kVersionOffset = 0;
constexpr size_t kKindOffset = 1;
constexpr size_t kSenderOffset = 2;
constexpr size_t kCounterOffset = 4;
constexpr size_t kPayloadOffset = 8;
constexpr size_t kPayloadSize = 8;
constexpr size_t kAuthCodeOffset = kPayloadOffset + kPayloadSize;
constexpr size_t kAuthCodeSize = 8;
static_assert(kAuthCodeOffset + kAuthCodeSize == kPacketSize, "frame layout and size disagree");
static_assert(kAuthCodeOffset == kPacketContentSize,
              "a packet's content is its frame up to the authentication code");
static_assert(kPacketSize <= kMaxPacketSize, "a packet must fit a receive buffer");

constexpr uint8_t kReadingFlagStormMode = 0x01;
constexpr uint8_t kReadingFlagLevelRepeated = 0x02;
constexpr uint8_t kAlertFlagRefresh = 0x01;
constexpr uint8_t kAlertFlagLevelRepeated = 0x02;
constexpr uint8_t kHeartbeatFlagTamper = 0x01;
constexpr uint8_t kHeartbeatFlagAlertRefreshLost = 0x02;
constexpr uint8_t kHeartbeatFlagHasLastLevel = 0x04;
constexpr uint8_t kHeartbeatFlagLastLevelRepeated = 0x08;

constexpr uint32_t kCounterMax = 0xFFFFFFFFu;

void put16(uint8_t* at, uint16_t value) {
  at[0] = static_cast<uint8_t>(value >> 8);
  at[1] = static_cast<uint8_t>(value);
}

void put32(uint8_t* at, uint32_t value) {
  put16(at, static_cast<uint16_t>(value >> 16));
  put16(at + 2, static_cast<uint16_t>(value));
}

uint16_t get16(const uint8_t* at) { return static_cast<uint16_t>((at[0] << 8) | at[1]); }

uint32_t get32(const uint8_t* at) {
  return (static_cast<uint32_t>(get16(at)) << 16) | get16(at + 2);
}

bool isAlertLevel(uint8_t value) { return value <= static_cast<uint8_t>(AlertLevel::Evacuate); }

// Writes the payload for the packet's kind. Returns false for a packet that makes no sense.
bool writePayload(const Packet& packet, uint8_t* payload) {
  std::memset(payload, 0, kPayloadSize);
  switch (packet.kind) {
    case PacketKind::Reading: {
      const ReadingPayload& reading = packet.reading;
      if (!isAlertLevel(static_cast<uint8_t>(reading.alertLevel))) return false;
      put16(payload, reading.levelMm);
      put16(payload + 2, static_cast<uint16_t>(reading.rateOfRiseMmPerMin));
      put16(payload + 4, reading.rainfallTenthsMm);
      payload[6] = static_cast<uint8_t>(reading.alertLevel);
      if (reading.stormMode) payload[7] |= kReadingFlagStormMode;
      if (reading.levelRepeated) payload[7] |= kReadingFlagLevelRepeated;
      return true;
    }
    case PacketKind::Alert: {
      const AlertPayload& alert = packet.alert;
      if (!isAlertLevel(static_cast<uint8_t>(alert.alertLevel))) return false;
      payload[0] = static_cast<uint8_t>(alert.alertLevel);
      put16(payload + 1, alert.levelMm);
      put16(payload + 3, static_cast<uint16_t>(alert.rateOfRiseMmPerMin));
      if (alert.refresh) payload[5] |= kAlertFlagRefresh;
      if (alert.levelRepeated) payload[5] |= kAlertFlagLevelRepeated;
      return true;
    }
    case PacketKind::Acknowledgement: {
      const AcknowledgementPayload& acknowledgement = packet.acknowledgement;
      if (!isAlertLevel(static_cast<uint8_t>(acknowledgement.alertLevel))) return false;
      put16(payload, acknowledgement.alertSender);
      put32(payload + 2, acknowledgement.alertCounter);
      payload[6] = static_cast<uint8_t>(acknowledgement.alertLevel);
      return true;
    }
    case PacketKind::Heartbeat: {
      const HeartbeatPayload& heartbeat = packet.heartbeat;
      put16(payload, heartbeat.batteryMillivolts);
      put16(payload + 2, static_cast<uint16_t>(heartbeat.signalStrengthDbm));
      put16(payload + 4, heartbeat.lastLevelMm);
      payload[6] = heartbeat.discardedReadings;
      if (heartbeat.tamper) payload[7] |= kHeartbeatFlagTamper;
      if (heartbeat.alertRefreshLost) payload[7] |= kHeartbeatFlagAlertRefreshLost;
      if (heartbeat.hasLastLevel) payload[7] |= kHeartbeatFlagHasLastLevel;
      if (heartbeat.lastLevelRepeated) payload[7] |= kHeartbeatFlagLastLevelRepeated;
      return true;
    }
    case PacketKind::AllClear:
      put16(payload, packet.allClear.wadiNode);
      put32(payload + 2, packet.allClear.command);
      return true;
    case PacketKind::Test:
      return true;
  }
  return false;
}

// Fills in the payload for the kind byte. Returns false for a payload that makes no sense.
bool readPayload(uint8_t kind, const uint8_t* payload, Packet& packet) {
  switch (static_cast<PacketKind>(kind)) {
    case PacketKind::Reading:
      if (!isAlertLevel(payload[6])) return false;
      packet.reading.levelMm = get16(payload);
      packet.reading.rateOfRiseMmPerMin = static_cast<int16_t>(get16(payload + 2));
      packet.reading.rainfallTenthsMm = get16(payload + 4);
      packet.reading.alertLevel = static_cast<AlertLevel>(payload[6]);
      packet.reading.stormMode = (payload[7] & kReadingFlagStormMode) != 0;
      packet.reading.levelRepeated = (payload[7] & kReadingFlagLevelRepeated) != 0;
      break;
    case PacketKind::Alert:
      if (!isAlertLevel(payload[0])) return false;
      packet.alert.alertLevel = static_cast<AlertLevel>(payload[0]);
      packet.alert.levelMm = get16(payload + 1);
      packet.alert.rateOfRiseMmPerMin = static_cast<int16_t>(get16(payload + 3));
      packet.alert.refresh = (payload[5] & kAlertFlagRefresh) != 0;
      packet.alert.levelRepeated = (payload[5] & kAlertFlagLevelRepeated) != 0;
      break;
    case PacketKind::Acknowledgement:
      if (!isAlertLevel(payload[6])) return false;
      packet.acknowledgement.alertSender = get16(payload);
      packet.acknowledgement.alertCounter = get32(payload + 2);
      packet.acknowledgement.alertLevel = static_cast<AlertLevel>(payload[6]);
      break;
    case PacketKind::Heartbeat:
      packet.heartbeat.batteryMillivolts = get16(payload);
      packet.heartbeat.signalStrengthDbm = static_cast<int16_t>(get16(payload + 2));
      packet.heartbeat.lastLevelMm = get16(payload + 4);
      packet.heartbeat.discardedReadings = payload[6];
      packet.heartbeat.tamper = (payload[7] & kHeartbeatFlagTamper) != 0;
      packet.heartbeat.alertRefreshLost = (payload[7] & kHeartbeatFlagAlertRefreshLost) != 0;
      packet.heartbeat.hasLastLevel = (payload[7] & kHeartbeatFlagHasLastLevel) != 0;
      packet.heartbeat.lastLevelRepeated =
          (payload[7] & kHeartbeatFlagLastLevelRepeated) != 0;
      break;
    case PacketKind::AllClear:
      packet.allClear.wadiNode = get16(payload);
      packet.allClear.command = get32(payload + 2);
      break;
    case PacketKind::Test:
      break;
    default:
      return false;
  }
  packet.kind = static_cast<PacketKind>(kind);
  return true;
}

// Computes the authentication code of a frame's header and payload.
void writeAuthCode(const SiteAuth& auth, const uint8_t* frame, uint8_t* authCode) {
  uint8_t digest[kSha256DigestSize];
  hmacSha256(auth.key, kSiteKeySize, frame, kAuthCodeOffset, digest);
  std::memcpy(authCode, digest, kAuthCodeSize);
}

}  // namespace

// The same bytes as the start of the frame PacketCodec::encode builds for the packet.
bool writePacketContent(const Packet& packet, uint8_t* content) {
  if (!writePayload(packet, content + kPayloadOffset)) return false;
  content[kVersionOffset] = kProtocolVersion;
  content[kKindOffset] = static_cast<uint8_t>(packet.kind);
  put16(content + kSenderOffset, packet.sender);
  put32(content + kCounterOffset, packet.counter);
  return true;
}

bool readPacketContent(const uint8_t* content, Packet& packet) {
  if (content[kVersionOffset] != kProtocolVersion) return false;
  Packet read = Packet();
  read.sender = get16(content + kSenderOffset);
  read.counter = get32(content + kCounterOffset);
  if (!readPayload(content[kKindOffset], content + kPayloadOffset, read)) return false;
  packet = read;
  return true;
}

PacketCodec::PacketCodec(const SiteAuth& auth, NodeId self, PersistentStorage& storage)
    : auth_(auth), self_(self), storage_(storage), sendCounter_(0), sendCounterLoaded_(false) {
  if (auth_.nodeCount > kMaxSiteNodes) auth_.nodeCount = kMaxSiteNodes;
  for (size_t i = 0; i < kMaxSiteNodes; ++i) {
    lastAccepted_[i] = 0;
    lastAcceptedLoaded_[i] = false;
  }
}

size_t PacketCodec::encode(Packet& packet, uint8_t* buffer, size_t capacity) {
  if (capacity < kPacketSize) return 0;

  uint8_t frame[kPacketSize];
  if (!writePayload(packet, frame + kPayloadOffset)) return 0;

  const uint32_t storedAt = storageKey(StorageRecord::SendCounter, self_);
  if (!sendCounterLoaded_) {
    uint32_t stored = 0;
    if (storage_.load(storedAt, stored)) sendCounter_ = stored;
    sendCounterLoaded_ = true;
  }
  // A counter that wrapped around would be rejected as a replay, so it stops instead.
  if (sendCounter_ == kCounterMax) return 0;
  ++sendCounter_;
  // Stored before the packet can go out: a restart must never reuse a counter.
  storage_.store(storedAt, sendCounter_);

  packet.sender = self_;
  packet.counter = sendCounter_;
  frame[kVersionOffset] = kProtocolVersion;
  frame[kKindOffset] = static_cast<uint8_t>(packet.kind);
  put16(frame + kSenderOffset, packet.sender);
  put32(frame + kCounterOffset, packet.counter);
  writeAuthCode(auth_, frame, frame + kAuthCodeOffset);

  std::memcpy(buffer, frame, kPacketSize);
  return kPacketSize;
}

bool PacketCodec::decode(const uint8_t* data, size_t length, Packet& packet) {
  if (length != kPacketSize) return false;
  if (data[kVersionOffset] != kProtocolVersion) return false;

  uint8_t expected[kAuthCodeSize];
  writeAuthCode(auth_, data, expected);
  if (!constantTimeEqual(expected, data + kAuthCodeOffset, kAuthCodeSize)) return false;

  // From here on the frame was made by someone holding the site key.
  Packet decoded = Packet();
  decoded.sender = get16(data + kSenderOffset);
  decoded.counter = get32(data + kCounterOffset);
  // A node never hears itself, so its own identifier on the air is an echo or a replay.
  if (decoded.sender == self_) return false;
  const int node = indexOfNode(decoded.sender);
  if (node < 0) return false;
  if (!readPayload(data[kKindOffset], data + kPayloadOffset, decoded)) return false;

  const uint32_t storedAt = storageKey(StorageRecord::LastAcceptedCounter, decoded.sender);
  if (!lastAcceptedLoaded_[node]) {
    uint32_t stored = 0;
    if (storage_.load(storedAt, stored)) lastAccepted_[node] = stored;
    lastAcceptedLoaded_[node] = true;
  }
  if (decoded.counter <= lastAccepted_[node]) return false;
  lastAccepted_[node] = decoded.counter;
  storage_.store(storedAt, decoded.counter);

  packet = decoded;
  return true;
}

int PacketCodec::indexOfNode(NodeId node) const {
  for (size_t i = 0; i < auth_.nodeCount; ++i) {
    if (auth_.nodes[i] == node) return static_cast<int>(i);
  }
  return -1;
}

}  // namespace wadiwatch
