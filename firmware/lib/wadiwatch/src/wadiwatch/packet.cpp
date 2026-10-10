#include "wadiwatch/packet.h"

namespace wadiwatch {

namespace {

constexpr uint8_t kProtocolVersion = 1;
// Every packet starts with: version, kind, sender (2 bytes, big-endian).
constexpr size_t kHeaderSize = 4;
// Header, alert level.
constexpr size_t kAlertPacketSize = kHeaderSize + 1;
// Header, battery millivolts (2 bytes), signal strength, flags, last level in cm (2 bytes).
constexpr size_t kHeartbeatPacketSize = kHeaderSize + 6;

constexpr uint8_t kHeartbeatFlagTamper = 0x01;
constexpr uint8_t kHeartbeatFlagHasLastLevel = 0x02;
constexpr uint8_t kHeartbeatKnownFlags = kHeartbeatFlagTamper | kHeartbeatFlagHasLastLevel;

void putUint16(uint8_t* buffer, uint16_t value) {
  buffer[0] = static_cast<uint8_t>(value >> 8);
  buffer[1] = static_cast<uint8_t>(value & 0xFF);
}

uint16_t getUint16(const uint8_t* data) {
  return static_cast<uint16_t>((data[0] << 8) | data[1]);
}

size_t packetSize(PacketKind kind) {
  switch (kind) {
    case PacketKind::Alert:
      return kAlertPacketSize;
    case PacketKind::Heartbeat:
      return kHeartbeatPacketSize;
  }
  return 0;
}

}  // namespace

size_t encodePacket(const Packet& packet, uint8_t* buffer, size_t capacity) {
  const size_t size = packetSize(packet.kind);
  if (size == 0 || capacity < size) return 0;
  buffer[0] = kProtocolVersion;
  buffer[1] = static_cast<uint8_t>(packet.kind);
  putUint16(buffer + 2, packet.sender);
  uint8_t* body = buffer + kHeaderSize;
  if (packet.kind == PacketKind::Alert) {
    body[0] = static_cast<uint8_t>(packet.alertLevel);
  } else {
    const Heartbeat& heartbeat = packet.heartbeat;
    putUint16(body, heartbeat.batteryMillivolts);
    body[2] = static_cast<uint8_t>(heartbeat.signalStrengthDbm);
    body[3] = static_cast<uint8_t>((heartbeat.tamper ? kHeartbeatFlagTamper : 0) |
                                   (heartbeat.hasLastLevel ? kHeartbeatFlagHasLastLevel : 0));
    putUint16(body + 4, static_cast<uint16_t>(heartbeat.lastLevelCm));
  }
  return size;
}

bool decodePacket(const uint8_t* data, size_t length, Packet& packet) {
  if (length < kHeaderSize) return false;
  if (data[0] != kProtocolVersion) return false;
  const uint8_t* body = data + kHeaderSize;
  Packet decoded = Packet();
  decoded.sender = getUint16(data + 2);
  if (data[1] == static_cast<uint8_t>(PacketKind::Alert)) {
    if (length != kAlertPacketSize) return false;
    if (body[0] > static_cast<uint8_t>(AlertLevel::Evacuate)) return false;
    decoded.kind = PacketKind::Alert;
    decoded.alertLevel = static_cast<AlertLevel>(body[0]);
  } else if (data[1] == static_cast<uint8_t>(PacketKind::Heartbeat)) {
    if (length != kHeartbeatPacketSize) return false;
    if ((body[3] & ~kHeartbeatKnownFlags) != 0) return false;
    decoded.kind = PacketKind::Heartbeat;
    decoded.heartbeat.batteryMillivolts = getUint16(body);
    decoded.heartbeat.signalStrengthDbm = static_cast<int8_t>(body[2]);
    decoded.heartbeat.tamper = (body[3] & kHeartbeatFlagTamper) != 0;
    decoded.heartbeat.hasLastLevel = (body[3] & kHeartbeatFlagHasLastLevel) != 0;
    decoded.heartbeat.lastLevelCm = static_cast<int16_t>(getUint16(body + 4));
  } else {
    return false;
  }
  packet = decoded;
  return true;
}

}  // namespace wadiwatch
