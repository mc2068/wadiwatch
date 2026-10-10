// ESP32 implementations of the ports shared by every device.
#pragma once

#include <esp_timer.h>

#include "wadiwatch/ports.h"

class EspClock : public wadiwatch::Clock {
 public:
  uint64_t nowMs() const override { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }
};

// Stands in for the LoRa radio until its adapter is written: sends nothing, hears nothing.
class UnconnectedRadio : public wadiwatch::Radio {
 public:
  void send(const uint8_t*, size_t) override {}
  size_t receive(uint8_t*, size_t) override { return 0; }
};

// Stands in for flash storage until its adapter is written: remembers nothing across a restart.
class UnconnectedStorage : public wadiwatch::PersistentStorage {
 public:
  bool load(uint32_t, uint32_t&) override { return false; }
  void store(uint32_t, uint32_t) override {}
};
