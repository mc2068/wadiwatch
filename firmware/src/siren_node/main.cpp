// Siren node firmware entry point: wires the core to the ESP32.
#include <Arduino.h>

#include "../esp32/esp_ports.h"
#include "wadiwatch/siren_node.h"
#include "wadiwatch/site_config.h"

namespace {

// Stands in for the siren and strobe drivers until their adapter is written: reports to the
// serial port.
class SerialSirenOutput : public wadiwatch::SirenOutput {
 public:
  void setSiren(bool on) override { Serial.println(on ? "SIREN ON" : "SIREN OFF"); }
  void setStrobe(bool on) override { Serial.println(on ? "STROBE ON" : "STROBE OFF"); }
};

// Stands in for the button under the cover until its adapter is written: never pressed.
class UnconnectedButton : public wadiwatch::Button {
 public:
  bool wasPressed() override { return false; }
};

EspClock espClock;
UnconnectedRadio radio;
SerialSirenOutput siren;
UnconnectedStorage storage;
UnmeasuredNodeHealth nodeHealth;
UnconnectedButton button;
wadiwatch::SirenNode node(wadiwatch::prototypeSite().sirenNode,
                          wadiwatch::prototypeSite().auth,
                          espClock,
                          radio,
                          siren,
                          storage,
                          nodeHealth,
                          button);

}  // namespace

void setup() { Serial.begin(115200); }

void loop() {
  node.tick();
  delay(10);
}
