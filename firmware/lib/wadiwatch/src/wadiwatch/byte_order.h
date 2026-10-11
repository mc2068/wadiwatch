// Multi-byte numbers as the devices write them out: big-endian, in radio frames, in
// stored messages and wherever else bytes leave a core.
#pragma once

#include <cstdint>

namespace wadiwatch {

inline void put16(uint8_t* at, uint16_t value) {
  at[0] = static_cast<uint8_t>(value >> 8);
  at[1] = static_cast<uint8_t>(value);
}

inline void put32(uint8_t* at, uint32_t value) {
  put16(at, static_cast<uint16_t>(value >> 16));
  put16(at + 2, static_cast<uint16_t>(value));
}

inline uint16_t get16(const uint8_t* at) { return static_cast<uint16_t>((at[0] << 8) | at[1]); }

inline uint32_t get32(const uint8_t* at) {
  return (static_cast<uint32_t>(get16(at)) << 16) | get16(at + 2);
}

}  // namespace wadiwatch
