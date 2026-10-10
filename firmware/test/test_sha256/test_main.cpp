// SHA-256 and HMAC-SHA-256 checked against published vectors
// (FIPS 180-4 examples and RFC 4231 test cases).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unity.h>

#include "wadiwatch/sha256.h"

using wadiwatch::Sha256;
using wadiwatch::hmacSha256;
using wadiwatch::kSha256DigestSize;

void setUp() {}
void tearDown() {}

using Bytes = std::vector<uint8_t>;

static Bytes bytesOf(const std::string& text) { return Bytes(text.begin(), text.end()); }

static std::string hexOf(const uint8_t* data, size_t length) {
  std::string hex;
  char pair[3];
  for (size_t i = 0; i < length; ++i) {
    std::snprintf(pair, sizeof(pair), "%02x", data[i]);
    hex += pair;
  }
  return hex;
}

static std::string sha256Hex(const Bytes& message) {
  uint8_t digest[kSha256DigestSize];
  Sha256 hash;
  hash.update(message.data(), message.size());
  hash.finish(digest);
  return hexOf(digest, sizeof(digest));
}

static std::string hmacHex(const Bytes& key, const Bytes& message) {
  uint8_t digest[kSha256DigestSize];
  hmacSha256(key.data(), key.size(), message.data(), message.size(), digest);
  return hexOf(digest, sizeof(digest));
}

static void test_sha256_matches_the_fips_180_examples() {
  struct Case {
    Bytes message;
    const char* digest;
  };
  const Case cases[] = {
      {bytesOf(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {bytesOf("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
      {bytesOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
       "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
      {Bytes(1000000, 'a'), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
  };
  for (const Case& c : cases) {
    TEST_ASSERT_EQUAL_STRING(c.digest, sha256Hex(c.message).c_str());
  }
}

static void test_sha256_gives_the_same_digest_however_the_message_is_split() {
  const Bytes message = bytesOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
  const std::string whole = sha256Hex(message);

  for (size_t split = 0; split <= message.size(); ++split) {
    uint8_t digest[kSha256DigestSize];
    Sha256 hash;
    hash.update(message.data(), split);
    hash.update(message.data() + split, message.size() - split);
    hash.finish(digest);
    TEST_ASSERT_EQUAL_STRING(whole.c_str(), hexOf(digest, sizeof(digest)).c_str());
  }
}

static void test_hmac_sha256_matches_the_rfc_4231_test_cases() {
  struct Case {
    Bytes key;
    Bytes message;
    const char* digest;
  };
  const Case cases[] = {
      // Test case 1
      {Bytes(20, 0x0b), bytesOf("Hi There"),
       "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"},
      // Test case 2: a key shorter than the digest
      {bytesOf("Jefe"), bytesOf("what do ya want for nothing?"),
       "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"},
      // Test case 3
      {Bytes(20, 0xaa), Bytes(50, 0xdd),
       "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"},
      // Test case 6: a key longer than the hash block is hashed first
      {Bytes(131, 0xaa), bytesOf("Test Using Larger Than Block-Size Key - Hash Key First"),
       "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"},
  };
  for (const Case& c : cases) {
    TEST_ASSERT_EQUAL_STRING(c.digest, hmacHex(c.key, c.message).c_str());
  }
}

static void test_hmac_sha256_truncates_to_the_leading_bytes() {
  // RFC 4231 test case 5: the code is the first 128 bits of the full output.
  const std::string full = hmacHex(Bytes(20, 0x0c), bytesOf("Test With Truncation"));

  TEST_ASSERT_EQUAL_STRING("a3b6167473100ee06e0c796c2955552b", full.substr(0, 32).c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_sha256_matches_the_fips_180_examples);
  RUN_TEST(test_sha256_gives_the_same_digest_however_the_message_is_split);
  RUN_TEST(test_hmac_sha256_matches_the_rfc_4231_test_cases);
  RUN_TEST(test_hmac_sha256_truncates_to_the_leading_bytes);
  return UNITY_END();
}
