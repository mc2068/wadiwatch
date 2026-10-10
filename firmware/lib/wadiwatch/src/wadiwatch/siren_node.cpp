#include "wadiwatch/siren_node.h"

#include "wadiwatch/packet.h"

namespace wadiwatch {

SirenNode::SirenNode(Radio& radio, SirenOutput& siren) : radio_(radio), siren_(siren) {}

void SirenNode::tick() {
  uint8_t frame[kMaxPacketSize];
  size_t length;
  while ((length = radio_.receive(frame, sizeof(frame))) > 0) {
    Packet packet;
    if (!decodePacket(frame, length, packet)) continue;
    if (packet.kind == PacketKind::Alert && packet.alertLevel >= AlertLevel::Warning) {
      siren_.setSiren(true);
    }
  }
}

}  // namespace wadiwatch
