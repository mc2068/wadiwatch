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

EspClock espClock;
UnconnectedLevelSensor levelSensor;
UnconnectedRadio radio;
wadiwatch::WadiNode node(wadiwatch::prototypeSite().wadiNode, espClock, levelSensor, radio);

}  // namespace

void setup() { Serial.begin(115200); }

void loop() {
  node.tick();
  delay(100);
}
