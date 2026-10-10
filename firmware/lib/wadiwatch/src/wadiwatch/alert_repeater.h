// Repeats of an alert. A wadi node transmits each alert several times, a random time
// apart, so that one lost transmission or one collision does not lose the alert.
// Every transmission carries the same bytes: receivers act on the first copy they
// hear and their packet codec refuses the others. The gateway repeats each all
// clear packet in the same way.
#pragma once

#include <cstddef>
#include <cstdint>

#include "wadiwatch/packet.h"
#include "wadiwatch/ports.h"

namespace wadiwatch {

struct AlertRepeatConfig {
  // How many times each alert is transmitted in all, the first time included.
  uint8_t transmissions;
  // The wait before each further transmission is drawn at random between these
  // two. The shorter one must be longer than a packet's time on the air.
  uint32_t minSpacingMs;
  uint32_t maxSpacingMs;
};

// Keeps the alert being repeated and says when its next transmission is due.
// It does not transmit: the wadi node does, through its one send path.
class AlertRepeater {
 public:
  AlertRepeater(const AlertRepeatConfig& config, RandomSource& random);

  // Call when an alert has just been transmitted for the first time: plans its
  // further transmissions. An older alert still being repeated is dropped, since
  // receivers that hear the newer one would refuse it anyway.
  void start(const uint8_t* frame, size_t length, uint64_t nowMs);

  // Drops the transmissions still to come: what was being repeated no longer holds.
  void cancel() { remaining_ = 0; }

  // Whether a transmission is still to come. It is due at dueAtMs().
  bool pending() const { return remaining_ > 0; }
  uint64_t dueAtMs() const { return dueAtMs_; }

  // When a transmission is due, returns the frame to transmit, sets its length
  // and plans the one after it. Returns nullptr when nothing is due.
  const uint8_t* takeDue(uint64_t nowMs, size_t& length);

 private:
  uint32_t drawSpacingMs();

  AlertRepeatConfig config_;
  RandomSource& random_;
  uint8_t frame_[kMaxPacketSize];
  size_t length_ = 0;
  uint8_t remaining_ = 0;
  uint64_t dueAtMs_ = 0;
};

}  // namespace wadiwatch
