// Wadi node core: measures the water level and decides the alert level itself.
#pragma once

#include <cstdint>

#include "wadiwatch/packet.h"
#include "wadiwatch/ports.h"
#include "wadiwatch/site_config.h"

namespace wadiwatch {

class WadiNode {
 public:
  WadiNode(const WadiNodeConfig& config, Clock& clock, LevelSensor& sensor, Radio& radio);

  // Call repeatedly; takes a measurement whenever one is due.
  void tick();

 private:
  void measure();
  void sendAlert();

  WadiNodeConfig config_;
  Clock& clock_;
  LevelSensor& sensor_;
  Radio& radio_;
  uint64_t nextMeasurementAtMs_ = 0;
  AlertLevel alertLevel_ = AlertLevel::None;
};

}  // namespace wadiwatch
