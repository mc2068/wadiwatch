// Wadi node core: measures the water level and decides the alert level itself.
#pragma once

#include <cstdint>

#include "wadiwatch/operating_mode.h"
#include "wadiwatch/packet.h"
#include "wadiwatch/ports.h"
#include "wadiwatch/rain_window.h"
#include "wadiwatch/rate_of_rise.h"
#include "wadiwatch/site_config.h"

namespace wadiwatch {

class WadiNode {
 public:
  WadiNode(const WadiNodeConfig& config,
           const SiteAuth& auth,
           Clock& clock,
           SleepControl& sleep,
           LevelSensor& sensor,
           RainGauge& rainGauge,
           NodeHealth& health,
           Radio& radio,
           PersistentStorage& storage,
           // Optional hardware: without it the level is not corrected for temperature.
           TemperatureSensor* temperature = nullptr);

  // Call whenever the device is awake. Does whatever is due (counting rain,
  // measuring, sending a heartbeat), then asks to sleep until the next thing is
  // due. Safe to call early: a rain-gauge tip wakes the device before its time.
  void tick();

 private:
  void countRain(uint64_t nowMs);
  void measure(uint64_t nowMs);
  void raiseAlertLevel(uint64_t nowMs);
  void sendAlert();
  void sendReading(uint64_t nowMs);
  void sendHeartbeat();
  void send(Packet& packet);
  void sleepUntilNextDue(uint64_t nowMs);

  WadiNodeConfig config_;
  Clock& clock_;
  SleepControl& sleep_;
  LevelSensor& sensor_;
  RainGauge& rainGauge_;
  NodeHealth& health_;
  Radio& radio_;
  TemperatureSensor* temperature_;
  PacketCodec codec_;
  ModeController mode_;
  RainWindow rain_;
  RateOfRise rise_;
  bool started_ = false;
  uint64_t nextMeasurementAtMs_ = 0;
  uint64_t nextHeartbeatAtMs_ = 0;
  uint32_t tipsCounted_ = 0;
  bool hasLevel_ = false;
  float levelCm_ = 0;
  // A storm-mode measurement has been taken and not yet sent as a reading.
  bool readingDue_ = false;
  // Pings discarded since the last heartbeat. Stops counting at its largest value.
  uint8_t discardedReadings_ = 0;
  AlertLevel alertLevel_ = AlertLevel::None;
};

}  // namespace wadiwatch
