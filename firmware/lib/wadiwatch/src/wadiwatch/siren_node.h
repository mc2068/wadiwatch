// Siren node core: answers alerts heard over the radio with the siren and the strobe.
// Warning sounds siren pattern 1 (intermittent), Evacuate a continuous siren;
// the strobe is on in both cases. Watch produces no output.
#pragma once

#include <cstdint>

#include "wadiwatch/packet.h"
#include "wadiwatch/ports.h"
#include "wadiwatch/site_config.h"

namespace wadiwatch {

class SirenNode {
 public:
  SirenNode(const SirenNodeConfig& config, Clock& clock, Radio& radio, SirenOutput& output);

  // Call repeatedly, at least as often as the shortest phase of the intermittent
  // pattern; handles every frame received since the last call and keeps the
  // siren pattern in time.
  void tick();

 private:
  void handleReceivedFrames(uint64_t nowMs);
  void driveOutput(uint64_t nowMs);

  SirenNodeConfig config_;
  Clock& clock_;
  Radio& radio_;
  SirenOutput& output_;
  // Highest alert level heard. Only ever rises.
  AlertLevel alertLevel_ = AlertLevel::None;
  uint64_t intermittentStartedAtMs_ = 0;
  bool sirenOn_ = false;
  bool strobeOn_ = false;
};

}  // namespace wadiwatch
