#include "wadiwatch/downlink_command.h"

#include <cstring>

#include "wadiwatch/byte_order.h"
#include "wadiwatch/gateway_contract.h"
#include "wadiwatch/sha256.h"

namespace wadiwatch {

namespace {

const char kAllClearKind[] = "all_clear";
constexpr uint32_t kMaxWadiId = 255;
constexpr uint32_t kMaxUint32 = 0xFFFFFFFFu;
// Longer than any field name or word of the contract.
constexpr size_t kMaxWordLength = 16;

// What the authentication code of a command is computed over, multi-byte fields
// big-endian:
//
//   0      contract version
//   1      kind of command: 1 is the all clear
//   2-5    command number
//   6-9    ts
//   10     wadi
constexpr uint8_t kAllClearKindCode = 1;
constexpr size_t kSignedVersionOffset = 0;
constexpr size_t kSignedKindOffset = 1;
constexpr size_t kSignedCommandOffset = 2;
constexpr size_t kSignedIssuedAtOffset = 6;
constexpr size_t kSignedWadiOffset = 10;
constexpr size_t kSignedSize = 11;
static_assert(2 * kSha256DigestSize == kCommandAuthCodeLength,
              "the authentication code is the whole digest in hexadecimal");

// Walks over the text of one command. Every read fails for good once the text is
// not what was asked for.
class Scanner {
 public:
  Scanner(const char* text, size_t length) : text_(text), length_(length) {}

  // The next character that is not white space must be `expected`.
  bool take(char expected) {
    skipSpace();
    if (at_ >= length_ || text_[at_] != expected) return false;
    ++at_;
    return true;
  }

  // Whether the next character that is not white space is `expected`; it stays.
  bool next(char expected) {
    skipSpace();
    return at_ < length_ && text_[at_] == expected;
  }

  // A string of plain characters: the contract's names and words have no escapes.
  bool word(char* word, size_t capacity) {
    if (!take('"')) return false;
    size_t count = 0;
    while (at_ < length_ && text_[at_] != '"') {
      const char c = text_[at_++];
      if (c == '\\' || static_cast<unsigned char>(c) < 0x20 || count + 1 >= capacity) return false;
      word[count++] = c;
    }
    word[count] = '\0';
    return take('"');
  }

  // A string of exactly `length` hexadecimal digits, the letters in lower case.
  bool hex(char* digits, size_t length) {
    if (!take('"')) return false;
    for (size_t i = 0; i < length; ++i) {
      if (at_ >= length_) return false;
      const char c = text_[at_++];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
      digits[i] = c;
    }
    digits[length] = '\0';
    return take('"');
  }

  // A whole number without sign, fraction or exponent, written without leading
  // zeros, that fits 32 bits.
  bool number(uint32_t& value) {
    skipSpace();
    const size_t start = at_;
    uint64_t read = 0;
    while (at_ < length_ && text_[at_] >= '0' && text_[at_] <= '9') {
      read = read * 10 + static_cast<uint64_t>(text_[at_] - '0');
      if (read > kMaxUint32) return false;
      ++at_;
    }
    if (at_ == start) return false;
    if (text_[start] == '0' && at_ - start > 1) return false;
    // "12.5", "1e3" and "12abc" are not whole numbers.
    if (at_ < length_ && text_[at_] != ',' && text_[at_] != '}' && !isSpace(text_[at_])) {
      return false;
    }
    value = static_cast<uint32_t>(read);
    return true;
  }

  bool atEnd() {
    skipSpace();
    return at_ == length_;
  }

 private:
  static bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

  void skipSpace() {
    while (at_ < length_ && isSpace(text_[at_])) ++at_;
  }

  const char* text_;
  size_t length_;
  size_t at_ = 0;
};

}  // namespace

void writeCommandAuthCode(const CommandAuth& auth,
                          const AllClearCommand& command,
                          CommandAuthCode& code) {
  uint8_t fields[kSignedSize];
  fields[kSignedVersionOffset] = static_cast<uint8_t>(kContractVersion);
  fields[kSignedKindOffset] = kAllClearKindCode;
  put32(fields + kSignedCommandOffset, command.command);
  put32(fields + kSignedIssuedAtOffset, command.issuedAt);
  fields[kSignedWadiOffset] = command.wadi;

  uint8_t digest[kSha256DigestSize];
  hmacSha256(auth.key, kCommandKeySize, fields, sizeof(fields), digest);
  const char kDigits[] = "0123456789abcdef";
  for (size_t i = 0; i < kSha256DigestSize; ++i) {
    code.text[2 * i] = kDigits[digest[i] >> 4];
    code.text[2 * i + 1] = kDigits[digest[i] & 0x0F];
  }
  code.text[kCommandAuthCodeLength] = '\0';
}

bool readAllClearCommand(const char* text,
                         size_t length,
                         const CommandAuth& auth,
                         AllClearCommand& command) {
  AllClearCommand read;
  CommandAuthCode given;
  if (!parseAllClearCommand(text, length, read, given)) return false;
  CommandAuthCode expected;
  writeCommandAuthCode(auth, read, expected);
  if (!constantTimeEqual(reinterpret_cast<const uint8_t*>(expected.text),
                         reinterpret_cast<const uint8_t*>(given.text),
                         kCommandAuthCodeLength)) {
    return false;
  }
  command = read;
  return true;
}

bool parseAllClearCommand(const char* text,
                          size_t length,
                          AllClearCommand& command,
                          CommandAuthCode& code) {
  if (text == nullptr || length == 0 || length > kMaxDownlinkCommandLength) return false;

  Scanner scanner(text, length);
  bool hasVersion = false;
  bool hasKind = false;
  bool hasCommand = false;
  bool hasIssuedAt = false;
  bool hasWadi = false;
  bool hasAuth = false;
  AllClearCommand read = AllClearCommand();
  CommandAuthCode readCode = CommandAuthCode();

  if (!scanner.take('{')) return false;
  if (!scanner.next('}')) {
    do {
      char name[kMaxWordLength];
      if (!scanner.word(name, sizeof(name)) || !scanner.take(':')) return false;

      if (std::strcmp(name, "kind") == 0) {
        char kind[kMaxWordLength];
        if (hasKind || !scanner.word(kind, sizeof(kind))) return false;
        if (std::strcmp(kind, kAllClearKind) != 0) return false;
        hasKind = true;
        continue;
      }
      if (std::strcmp(name, "auth") == 0) {
        if (hasAuth || !scanner.hex(readCode.text, kCommandAuthCodeLength)) return false;
        hasAuth = true;
        continue;
      }

      uint32_t value;
      if (!scanner.number(value)) return false;
      if (std::strcmp(name, "v") == 0) {
        if (hasVersion || value != kContractVersion) return false;
        hasVersion = true;
      } else if (std::strcmp(name, "command") == 0) {
        if (hasCommand || value == 0) return false;
        read.command = value;
        hasCommand = true;
      } else if (std::strcmp(name, "ts") == 0) {
        if (hasIssuedAt || value == 0) return false;
        read.issuedAt = value;
        hasIssuedAt = true;
      } else if (std::strcmp(name, "wadi") == 0) {
        if (hasWadi || value == 0 || value > kMaxWadiId) return false;
        read.wadi = static_cast<WadiId>(value);
        hasWadi = true;
      } else {
        // A field the contract does not list.
        return false;
      }
    } while (scanner.take(','));
  }
  if (!scanner.take('}') || !scanner.atEnd()) return false;
  if (!(hasVersion && hasKind && hasCommand && hasIssuedAt && hasWadi && hasAuth)) return false;

  command = read;
  code = readCode;
  return true;
}

}  // namespace wadiwatch
