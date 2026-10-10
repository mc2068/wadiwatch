// Radio packet format shared by every device.
#pragma once

#include <cstddef>
#include <cstdint>

namespace wadiwatch {

using NodeId = uint16_t;

enum class AlertLevel : uint8_t { None = 0, Watch = 1, Warning = 2, Evacuate = 3 };

enum class PacketKind : uint8_t { Alert = 1, Heartbeat = 2 };

// What a heartbeat packet carries.
struct Heartbeat {
  uint16_t batteryMillivolts;
  int8_t signalStrengthDbm;
  bool tamper;
  // False until the node has taken a level measurement.
  bool hasLastLevel;
  int16_t lastLevelCm;
};

struct Packet {
  PacketKind kind;
  NodeId sender;
  // Alert packets only.
  AlertLevel alertLevel;
  // Heartbeat packets only.
  Heartbeat heartbeat;
};

constexpr size_t kMaxPacketSize = 32;

// Returns the number of bytes written, or 0 if the buffer is too small.
size_t encodePacket(const Packet& packet, uint8_t* buffer, size_t capacity);

// Returns false for anything that is not a well-formed packet.
bool decodePacket(const uint8_t* data, size_t length, Packet& packet);

}  // namespace wadiwatch
