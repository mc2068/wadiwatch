// Siren node core: sounds the siren when an authenticated alert arrives over the radio.
#pragma once

#include "wadiwatch/packet.h"
#include "wadiwatch/ports.h"
#include "wadiwatch/site_config.h"

namespace wadiwatch {

class SirenNode {
 public:
  SirenNode(const SirenNodeConfig& config, const SiteAuth& auth, Radio& radio, SirenOutput& siren,
            PersistentStorage& storage);

  // Call repeatedly; handles every frame received since the last call.
  void tick();

 private:
  Radio& radio_;
  SirenOutput& siren_;
  PacketCodec codec_;
};

}  // namespace wadiwatch
