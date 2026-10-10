#include "wadiwatch/siren_node.h"

namespace wadiwatch {

SirenNode::SirenNode(const SirenNodeConfig& config, const SiteAuth& auth, Radio& radio,
                     SirenOutput& siren, PersistentStorage& storage)
    : radio_(radio), siren_(siren), codec_(auth, config.id, storage) {}

void SirenNode::tick() {
  uint8_t frame[kMaxPacketSize];
  size_t length;
  while ((length = radio_.receive(frame, sizeof(frame))) > 0) {
    Packet packet;
    if (!codec_.decode(frame, length, packet)) continue;
    if (packet.kind == PacketKind::Alert && packet.alert.alertLevel >= AlertLevel::Warning) {
      siren_.setSiren(true);
    }
  }
}

}  // namespace wadiwatch
