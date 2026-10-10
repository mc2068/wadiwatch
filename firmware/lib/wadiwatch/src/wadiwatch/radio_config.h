// Radio settings of a site, and how long a frame sent with them occupies the air.
#pragma once

#include <cstddef>
#include <cstdint>

namespace wadiwatch {

// LoRa settings shared by every node of a site. Starting values, to be tuned in range tests.
// Frames are sent with an explicit header and a CRC.
struct RadioConfig {
  uint32_t frequencyHz;
  int8_t transmitPowerDbm;
  // 7 to 12. Each step up reaches further and roughly doubles the time on air.
  uint8_t spreadingFactor;
  uint32_t bandwidthHz;
  // The coding rate is 4 over this: 5 to 8.
  uint8_t codingRateDenominator;
  uint16_t preambleSymbols;
  // The share of time one transmitter may occupy the band, measured over a period of this length.
  float dutyCycleLimitPercent;
  uint32_t dutyCyclePeriodMs;
};

// Time on air, in microseconds, of one frame of `frameBytes` bytes sent with these settings:
// the standard LoRa formula (Semtech SX1276 datasheet, section 4.1.1.7). Returns 0 for
// settings no LoRa radio accepts. Pure.
uint32_t loraTimeOnAirUs(const RadioConfig& radio, size_t frameBytes);

}  // namespace wadiwatch
