#include "wadiwatch/packet.h"

namespace wadiwatch {

namespace {

constexpr uint8_t kProtocolVersion = 1;
// version, kind, sender (2 bytes, big-endian), alert level
constexpr size_t kAlertPacketSize = 5;

}  // namespace

size_t encodePacket(const Packet& packet, uint8_t* buffer, size_t capacity) {
  if (capacity < kAlertPacketSize) return 0;
  buffer[0] = kProtocolVersion;
  buffer[1] = static_cast<uint8_t>(packet.kind);
  buffer[2] = static_cast<uint8_t>(packet.sender >> 8);
  buffer[3] = static_cast<uint8_t>(packet.sender & 0xFF);
  buffer[4] = static_cast<uint8_t>(packet.alertLevel);
  return kAlertPacketSize;
}

bool decodePacket(const uint8_t* data, size_t length, Packet& packet) {
  if (length != kAlertPacketSize) return false;
  if (data[0] != kProtocolVersion) return false;
  if (data[1] != static_cast<uint8_t>(PacketKind::Alert)) return false;
  if (data[4] > static_cast<uint8_t>(AlertLevel::Evacuate)) return false;
  packet.kind = PacketKind::Alert;
  packet.sender = static_cast<NodeId>((data[2] << 8) | data[3]);
  packet.alertLevel = static_cast<AlertLevel>(data[4]);
  return true;
}

}  // namespace wadiwatch
