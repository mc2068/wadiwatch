// Ports: the only way a device core touches the outside world.
// Firmware supplies hardware adapters; tests supply fakes.
#pragma once

#include <cstddef>
#include <cstdint>

namespace wadiwatch {

class Clock {
 public:
  virtual ~Clock() = default;
  virtual uint64_t nowMs() const = 0;
};

class LevelSensor {
 public:
  virtual ~LevelSensor() = default;
  // Distance from the sensor face down to the water surface (or the dry bed).
  // Returns false when no reading could be taken.
  virtual bool readDistanceCm(float& distanceCm) = 0;
};

class Radio {
 public:
  virtual ~Radio() = default;
  virtual void send(const uint8_t* data, size_t length) = 0;
  // Copies the next received frame into buffer and returns its length,
  // or 0 when nothing is waiting.
  virtual size_t receive(uint8_t* buffer, size_t capacity) = 0;
};

// The siren and the strobe beside it. The siren node core times the siren
// pattern itself and switches the siren on and off through this port.
class SirenOutput {
 public:
  virtual ~SirenOutput() = default;
  virtual void setSiren(bool on) = 0;
  virtual void setStrobe(bool on) = 0;
};

}  // namespace wadiwatch
