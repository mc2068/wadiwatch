// SHA-256 (FIPS 180-4) and HMAC-SHA-256 (RFC 2104), used to authenticate radio packets.
// Small and self-contained so the same code runs on the ESP32 and in the PC tests.
#pragma once

#include <cstddef>
#include <cstdint>

namespace wadiwatch {

constexpr size_t kSha256DigestSize = 32;
constexpr size_t kSha256BlockSize = 64;

class Sha256 {
 public:
  Sha256();

  void update(const uint8_t* data, size_t length);
  // Writes the digest. The object must not be used again afterwards.
  void finish(uint8_t digest[kSha256DigestSize]);

 private:
  void compress(const uint8_t block[kSha256BlockSize]);

  uint32_t state_[8];
  uint8_t buffer_[kSha256BlockSize];
  size_t buffered_;
  uint64_t totalBytes_;
};

void hmacSha256(const uint8_t* key, size_t keyLength, const uint8_t* message, size_t messageLength,
                uint8_t digest[kSha256DigestSize]);

// Compares two byte strings in time that does not depend on where they differ.
bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t length);

}  // namespace wadiwatch
