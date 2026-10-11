#include "wadiwatch/sha256.h"

#include <cstring>

namespace wadiwatch {

namespace {

const uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

const uint32_t kInitialState[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

const uint8_t kHmacInnerPad = 0x36;
const uint8_t kHmacOuterPad = 0x5c;

inline uint32_t rotateRight(uint32_t value, unsigned bits) {
  return (value >> bits) | (value << (32 - bits));
}

}  // namespace

Sha256::Sha256() : buffered_(0), totalBytes_(0) {
  std::memcpy(state_, kInitialState, sizeof(state_));
}

void Sha256::update(const uint8_t* data, size_t length) {
  totalBytes_ += length;
  while (length > 0) {
    size_t take = kSha256BlockSize - buffered_;
    if (take > length) take = length;
    std::memcpy(buffer_ + buffered_, data, take);
    buffered_ += take;
    data += take;
    length -= take;
    if (buffered_ == kSha256BlockSize) {
      compress(buffer_);
      buffered_ = 0;
    }
  }
}

void Sha256::finish(uint8_t digest[kSha256DigestSize]) {
  const uint64_t totalBits = totalBytes_ * 8;

  // Padding: a single 1 bit, zeros, then the message length in bits as 8 big-endian bytes.
  const size_t lengthFieldSize = 8;
  buffer_[buffered_++] = 0x80;
  if (buffered_ > kSha256BlockSize - lengthFieldSize) {
    std::memset(buffer_ + buffered_, 0, kSha256BlockSize - buffered_);
    compress(buffer_);
    buffered_ = 0;
  }
  std::memset(buffer_ + buffered_, 0, kSha256BlockSize - lengthFieldSize - buffered_);
  for (size_t i = 0; i < lengthFieldSize; ++i) {
    buffer_[kSha256BlockSize - 1 - i] = static_cast<uint8_t>(totalBits >> (8 * i));
  }
  compress(buffer_);

  for (size_t i = 0; i < 8; ++i) {
    digest[4 * i] = static_cast<uint8_t>(state_[i] >> 24);
    digest[4 * i + 1] = static_cast<uint8_t>(state_[i] >> 16);
    digest[4 * i + 2] = static_cast<uint8_t>(state_[i] >> 8);
    digest[4 * i + 3] = static_cast<uint8_t>(state_[i]);
  }
}

void Sha256::compress(const uint8_t block[kSha256BlockSize]) {
  uint32_t w[64];
  for (size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(block[4 * i]) << 24) |
           (static_cast<uint32_t>(block[4 * i + 1]) << 16) |
           (static_cast<uint32_t>(block[4 * i + 2]) << 8) | static_cast<uint32_t>(block[4 * i + 3]);
  }
  for (size_t i = 16; i < 64; ++i) {
    const uint32_t s0 = rotateRight(w[i - 15], 7) ^ rotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotateRight(w[i - 2], 17) ^ rotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
  uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
  for (size_t i = 0; i < 64; ++i) {
    const uint32_t s1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
    const uint32_t choose = (e & f) ^ (~e & g);
    const uint32_t t1 = h + s1 + choose + kRoundConstants[i] + w[i];
    const uint32_t s0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void hmacSha256(const uint8_t* key, size_t keyLength, const uint8_t* message, size_t messageLength,
                uint8_t digest[kSha256DigestSize]) {
  // A key longer than one block is replaced by its hash; a shorter one is padded with zeros.
  uint8_t blockKey[kSha256BlockSize];
  std::memset(blockKey, 0, sizeof(blockKey));
  if (keyLength > kSha256BlockSize) {
    Sha256 keyHash;
    keyHash.update(key, keyLength);
    keyHash.finish(blockKey);
  } else {
    std::memcpy(blockKey, key, keyLength);
  }

  uint8_t pad[kSha256BlockSize];
  for (size_t i = 0; i < kSha256BlockSize; ++i) pad[i] = blockKey[i] ^ kHmacInnerPad;
  uint8_t innerDigest[kSha256DigestSize];
  Sha256 inner;
  inner.update(pad, sizeof(pad));
  inner.update(message, messageLength);
  inner.finish(innerDigest);

  for (size_t i = 0; i < kSha256BlockSize; ++i) pad[i] = blockKey[i] ^ kHmacOuterPad;
  Sha256 outer;
  outer.update(pad, sizeof(pad));
  outer.update(innerDigest, sizeof(innerDigest));
  outer.finish(digest);
}

bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t length) {
  uint8_t difference = 0;
  for (size_t i = 0; i < length; ++i) difference |= static_cast<uint8_t>(a[i] ^ b[i]);
  return difference == 0;
}

}  // namespace wadiwatch
