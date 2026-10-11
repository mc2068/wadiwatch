// Siren node firmware entry point: wires the core to the ESP32.
#include <Arduino.h>

#include "../esp32/esp_ports.h"
#include "wadiwatch/siren_node.h"

namespace {

// Stands in for the siren driver until its adapter is written: reports to the serial port.
class SerialSirenOutput : public wadiwatch::SirenOutput {
 public:
  void setSiren(bool on) override { Serial.println(on ? "SIREN ON" : "SIREN OFF"); }
};

UnconnectedRadio radio;
SerialSirenOutput siren;
wadiwatch::SirenNode node(radio, siren);

}  // namespace

void setup() { Serial.begin(115200); }

void loop() {
  node.tick();
  delay(10);
}
