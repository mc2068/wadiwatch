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

// Lets a core ask for sleep once it has nothing left to do.
class SleepControl {
 public:
  virtual ~SleepControl() = default;
  // Asks to sleep for up to durationMs once the current tick returns.
  // The device may wake sooner (a rain-gauge tip wakes the wadi node, and so does a
  // frame arriving while its receiver is on), so the next tick can come before the
  // time is up.
  virtual void requestSleepMs(uint64_t durationMs) = 0;
};

class LevelSensor {
 public:
  virtual ~LevelSensor() = default;
  // One ultrasonic ping: the distance from the sensor face down to whatever
  // returned the echo, normally the water surface (or the dry bed). Unfiltered:
  // the core takes several pings per measurement and filters them itself.
  // Returns false when no reading could be taken.
  virtual bool readDistanceCm(float& distanceCm) = 0;
};

// The air temperature at the level sensor, used to correct the speed of sound.
// Optional hardware: a wadi node without it measures the level uncorrected.
class TemperatureSensor {
 public:
  virtual ~TemperatureSensor() = default;
  // Returns false when no trustworthy reading could be taken.
  virtual bool readTemperatureC(float& temperatureC) = 0;
};

class RainGauge {
 public:
  virtual ~RainGauge() = default;
  // Bucket tips counted since power-on. Counting continues while the processor sleeps.
  virtual uint32_t tipCount() = 0;
};

// The health values a node reports in its heartbeat.
class NodeHealth {
 public:
  virtual ~NodeHealth() = default;
  virtual uint16_t batteryMillivolts() = 0;
  // Strength of the last frame the radio received.
  virtual int8_t signalStrengthDbm() = 0;
  virtual bool tamperDetected() = 0;
};

class Radio {
 public:
  virtual ~Radio() = default;
  virtual void send(const uint8_t* data, size_t length) = 0;
  // Switches the receiver on or off. A radio listens from power-on; a device that
  // sleeps switches the receiver off to save energy, and on again for its receive
  // windows. Frames sent by others while the receiver is off are lost.
  virtual void setListening(bool listening) = 0;
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

// The test and silence button under the siren node's cover.
class Button {
 public:
  virtual ~Button() = default;
  // True once for each press of the button: a press is reported by one call
  // and not again. Debouncing is the adapter's business.
  virtual bool wasPressed() = 0;
};

// Chance, for whatever must not happen at a predictable moment.
class RandomSource {
 public:
  virtual ~RandomSource() = default;
  // A number picked evenly from 0 up to, but not including, `bound`.
  // `bound` is at least 1.
  virtual uint32_t below(uint32_t bound) = 0;
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
  // Alert level a siren node has latched from a wadi node, as the numeric value of
  // the AlertLevel (zero for none). Index: that wadi node's identifier.
  LatchedAlert = 3,
};

inline uint32_t storageKey(StorageRecord record, uint16_t index) {
  return (static_cast<uint32_t>(record) << 16) | index;
}

// The gateway's GSM modem, as far as SMS go.
class GsmModem {
 public:
  virtual ~GsmModem() = default;
  // Sends one SMS to one phone number. The number is in international format
  // (+216...) and the text is UTF-8; both end with a zero byte. Returns false
  // when the SMS could not be handed to the mobile network.
  virtual bool sendSms(const char* phoneNumber, const char* text) = 0;
};

// The time of day, which the gateway needs to date what it forwards. It comes from
// outside (the mobile network or NTP), so after power-on there is a while without it.
class WallClock {
 public:
  virtual ~WallClock() = default;
  // Unix time in whole seconds, UTC. Returns false while the time is not known.
  virtual bool nowUnixSeconds(uint32_t& unixSeconds) = 0;
};

// The gateway's connection to the backend (MQTT, see docs/gateway-contract.md).
// Only the way up exists so far. The way down (commands from the backend, such as
// the all clear) uses the same connection and belongs on this port too: add it as
// a method that hands over the next command received, the way Radio::receive
// hands over frames.
class Uplink {
 public:
  virtual ~Uplink() = default;
  // Publishes one message, `length` bytes of UTF-8 JSON, to the backend. Returns
  // true only once the message has been taken over (the broker confirmed it).
  // Returns false when the uplink is offline or the message was not confirmed;
  // the message may still have arrived, and the caller sends it again unchanged.
  virtual bool publish(const char* message, size_t length) = 0;
};

// Every slot of a SlotStorage holds this many bytes.
constexpr size_t kStorageSlotSize = 32;

// Numbered slots of kStorageSlotSize bytes that outlive restarts (flash, FRAM or a
// file): where the gateway keeps messages until the backend has them. The adapter
// only reads and writes whole slots; what they mean is the core's business.
class SlotStorage {
 public:
  virtual ~SlotStorage() = default;
  // How many slots there are. Does not change while the device runs.
  virtual size_t slotCount() = 0;
  // Copies a slot into data (kStorageSlotSize bytes). Returns false when the slot
  // cannot be read; a slot never written may return anything.
  virtual bool read(size_t slot, uint8_t* data) = 0;
  // Replaces a slot with data (kStorageSlotSize bytes). Returns false when the
  // slot could not be written.
  virtual bool write(size_t slot, const uint8_t* data) = 0;
};

}  // namespace wadiwatch
