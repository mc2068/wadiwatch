// Siren node core: sounds the siren when an alert arrives over the radio.
#pragma once

#include "wadiwatch/ports.h"

namespace wadiwatch {

class SirenNode {
 public:
  SirenNode(Radio& radio, SirenOutput& siren);

  // Call repeatedly; handles every frame received since the last call.
  void tick();

 private:
  Radio& radio_;
  SirenOutput& siren_;
};

}  // namespace wadiwatch
