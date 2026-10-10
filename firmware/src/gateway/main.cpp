// Gateway firmware entry point: wires the core to the ESP32.
#include <Arduino.h>

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

EspClock espClock;
UnconnectedRadio radio;
SerialGsmModem modem;
UnconnectedStorage storage;
// The committed recipient list holds fake numbers only.
wadiwatch::Gateway gateway(wadiwatch::prototypeSite().gateway,
                           wadiwatch::prototypeSite().auth,
                           wadiwatch::prototypeRecipients(),
                           espClock,
                           radio,
                           modem,
                           storage);

}  // namespace

void setup() { Serial.begin(115200); }

void loop() {
  gateway.tick();
  delay(10);
}
