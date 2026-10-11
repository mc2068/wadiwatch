#include "wadiwatch/radio_config.h"

namespace wadiwatch {

namespace {

constexpr uint8_t kMinSpreadingFactor = 7;
constexpr uint8_t kMaxSpreadingFactor = 12;
constexpr uint8_t kMinCodingRateDenominator = 5;
constexpr uint8_t kMaxCodingRateDenominator = 8;
constexpr uint64_t kMicrosPerSecond = 1000000;
// A symbol longer than this needs the low data rate optimisation (SF11 and SF12 at 125 kHz).
constexpr uint64_t kLongSymbolUs = 16000;
// Every frame has an explicit header and a CRC.
constexpr int32_t kCrcBits = 16;
constexpr int32_t kHeaderSymbolBits = 28;
constexpr int32_t kMinPayloadSymbols = 8;

}  // namespace

uint32_t loraTimeOnAirUs(const RadioConfig& radio, size_t frameBytes) {
  if (radio.bandwidthHz == 0) return 0;
  if (radio.spreadingFactor < kMinSpreadingFactor || radio.spreadingFactor > kMaxSpreadingFactor) {
    return 0;
  }
  if (radio.codingRateDenominator < kMinCodingRateDenominator ||
      radio.codingRateDenominator > kMaxCodingRateDenominator) {
    return 0;
  }

  const int32_t spreadingFactor = radio.spreadingFactor;
  const uint64_t chipsPerSymbol = static_cast<uint64_t>(1) << spreadingFactor;
  const uint64_t symbolUs = chipsPerSymbol * kMicrosPerSecond / radio.bandwidthHz;
  const int32_t lowDataRate = symbolUs > kLongSymbolUs ? 1 : 0;

  // The preamble is the configured symbols plus 4.25 symbols of synchronisation.
  const uint64_t preambleQuarterSymbols = 4u * radio.preambleSymbols + 17u;

  // Symbols after the preamble: 8, plus whole code words for the bits that do not fit in them.
  const int32_t bitsToCode = 8 * static_cast<int32_t>(frameBytes) - 4 * spreadingFactor +
                             kHeaderSymbolBits + kCrcBits;
  const int32_t bitsPerCodeWord = 4 * (spreadingFactor - 2 * lowDataRate);
  int32_t payloadSymbols = kMinPayloadSymbols;
  if (bitsToCode > 0) {
    const int32_t codeWords = (bitsToCode + bitsPerCodeWord - 1) / bitsPerCodeWord;
    payloadSymbols += codeWords * radio.codingRateDenominator;
  }

  const uint64_t quarterSymbols = preambleQuarterSymbols + 4u * static_cast<uint64_t>(payloadSymbols);
  return static_cast<uint32_t>(quarterSymbols * chipsPerSymbol * kMicrosPerSecond /
                               (4u * static_cast<uint64_t>(radio.bandwidthHz)));
}

}  // namespace wadiwatch
