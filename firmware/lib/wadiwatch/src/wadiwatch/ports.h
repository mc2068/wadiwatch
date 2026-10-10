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

class SirenOutput {
 public:
  virtual ~SirenOutput() = default;
  virtual void setSiren(bool on) = 0;
};

// Small values that must outlive deep sleep and restarts, such as message counters.
// Each device has its own storage.
class PersistentStorage {
 public:
  virtual ~PersistentStorage() = default;
  // Returns false when nothing has been stored under the key.
  virtual bool load(uint32_t key, uint32_t& value) = 0;
  virtual void store(uint32_t key, uint32_t value) = 0;
};

// What a stored value is. Every use of the storage takes its own record here,
// so two modules on the same device never share a key.
enum class StorageRecord : uint16_t {
  // Counter of the last packet this device sent. Index: this device's node identifier.
  SendCounter = 1,
  // Counter of the last packet accepted from another node. Index: that node's identifier.
  LastAcceptedCounter = 2,
};

inline uint32_t storageKey(StorageRecord record, uint16_t index) {
  return (static_cast<uint32_t>(record) << 16) | index;
}

}  // namespace wadiwatch
