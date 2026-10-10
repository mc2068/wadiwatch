// Wadi node firmware entry point: wires the core to the ESP32.
#include <Arduino.h>

#include "../esp32/esp_ports.h"
#include "wadiwatch/site_config.h"
#include "wadiwatch/wadi_node.h"

namespace {

// Stands in for the ultrasonic sensor until its adapter is written: never has a reading.
class UnconnectedLevelSensor : public wadiwatch::LevelSensor {
 public:
  bool readDistanceCm(float&) override { return false; }
};

// Stands in for the rain gauge until its adapter is written: never tips.
class UnconnectedRainGauge : public wadiwatch::RainGauge {
 public:
  uint32_t tipCount() override { return 0; }
};

// Stands in for the battery, signal and tamper readings until their adapters are written.
class UnmeasuredNodeHealth : public wadiwatch::NodeHealth {
 public:
  uint16_t batteryMillivolts() override { return 0; }
  int8_t signalStrengthDbm() override { return 0; }
  bool tamperDetected() override { return false; }
};

// Stands in for deep sleep until its adapter is written: the device stays awake
// and the core is simply ticked again before its time.
class StayAwake : public wadiwatch::SleepControl {
 public:
  void requestSleepMs(uint64_t) override {}
};

EspClock espClock;
StayAwake sleepControl;
UnconnectedLevelSensor levelSensor;
UnconnectedRainGauge rainGauge;
UnmeasuredNodeHealth nodeHealth;
UnconnectedRadio radio;
UnconnectedStorage storage;
wadiwatch::WadiNode node(wadiwatch::prototypeSite().wadiNode,
                         wadiwatch::prototypeSite().auth,
                         espClock,
                         sleepControl,
                         levelSensor,
                         rainGauge,
                         nodeHealth,
                         radio,
                         storage);

}  // namespace

void setup() { Serial.begin(115200); }

void loop() {
  node.tick();
  delay(100);
}
