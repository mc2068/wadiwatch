// Gateway firmware entry point: wires the core to the ESP32.
#include <Arduino.h>

#include <cstring>

#include "../esp32/esp_ports.h"
#include "wadiwatch/gateway.h"
#include "wadiwatch/recipients.h"
#include "wadiwatch/site_config.h"

namespace {

// Stands in for the GSM modem until its adapter is written: reports each SMS to the serial
// port and sends nothing.
class SerialGsmModem : public wadiwatch::GsmModem {
 public:
  bool sendSms(const char* phoneNumber, const char* text) override {
    Serial.print("SMS to ");
    Serial.print(phoneNumber);
    Serial.print(": ");
    Serial.println(text);
    return true;
  }
};

// Stands in for the time of day until the gateway has a network to get it from: the
// time is never known. The gateway then holds its messages, since it cannot date them.
class UnsetWallClock : public wadiwatch::WallClock {
 public:
  bool nowUnixSeconds(uint32_t&) override { return false; }
};

// Stands in for the uplink until its adapter is written: reports each message to the
// serial port and takes it, as a backend that is always reachable would. No command
// ever comes down it.
class SerialUplink : public wadiwatch::Uplink {
 public:
  bool publish(const char* message, size_t length) override {
    Serial.print("UPLINK ");
    Serial.write(reinterpret_cast<const uint8_t*>(message), length);
    Serial.println();
    return true;
  }

  size_t receive(char*, size_t) override { return 0; }
};

// Stands in for the flash that holds messages waiting for the uplink until its adapter
// is written: a few slots in memory, which a restart empties.
class MemorySlotStorage : public wadiwatch::SlotStorage {
 public:
  size_t slotCount() override { return kSlotCount; }

  bool read(size_t slot, uint8_t* data) override {
    if (slot >= kSlotCount) return false;
    std::memcpy(data, slots_[slot], wadiwatch::kStorageSlotSize);
    return true;
  }

  bool write(size_t slot, const uint8_t* data) override {
    if (slot >= kSlotCount) return false;
    std::memcpy(slots_[slot], data, wadiwatch::kStorageSlotSize);
    return true;
  }

 private:
  static const size_t kSlotCount = 64;
  uint8_t slots_[kSlotCount][wadiwatch::kStorageSlotSize] = {};
};

EspClock espClock;
UnconnectedRadio radio;
SerialGsmModem modem;
UnconnectedStorage storage;
UnsetWallClock wallClock;
SerialUplink uplink;
MemorySlotStorage messageStorage;
ArduinoRandom randomSource;
// The committed recipient list holds fake numbers only.
wadiwatch::Gateway gateway(wadiwatch::prototypeSite().gateway,
                           wadiwatch::prototypeSite().auth,
                           wadiwatch::prototypeCommandAuth(),
                           wadiwatch::prototypeRecipients(),
                           espClock,
                           radio,
                           modem,
                           storage,
                           wallClock,
                           uplink,
                           messageStorage,
                           randomSource);

}  // namespace

void setup() { Serial.begin(115200); }

void loop() {
  gateway.tick();
  delay(10);
}
