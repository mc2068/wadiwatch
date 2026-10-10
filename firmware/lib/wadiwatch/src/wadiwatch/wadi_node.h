// Wadi node core: measures the water level and decides the alert level itself.
#pragma once

#include <cstdint>

#include "wadiwatch/operating_mode.h"
#include "wadiwatch/packet.h"
#include "wadiwatch/ports.h"
#include "wadiwatch/rain_window.h"
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
           PersistentStorage& storage);

  // Call whenever the device is awake. Does whatever is due (counting rain,
  // measuring, sending a heartbeat), then asks to sleep until the next thing is
  // due. Safe to call early: a rain-gauge tip wakes the device before its time.
  void tick();

 private:
  void countRain(uint64_t nowMs);
  void measure(uint64_t nowMs);
  void raiseAlertLevel(uint64_t nowMs);
  void sendAlert();
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
  PacketCodec codec_;
  ModeController mode_;
  RainWindow rain_;
  bool started_ = false;
  uint64_t nextMeasurementAtMs_ = 0;
  uint64_t nextHeartbeatAtMs_ = 0;
  uint32_t tipsCounted_ = 0;
  bool hasLevel_ = false;
  float levelCm_ = 0;
  AlertLevel alertLevel_ = AlertLevel::None;
};

}  // namespace wadiwatch
