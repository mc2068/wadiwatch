// The command the backend sends down to the gateway, as docs/gateway-contract.md
// defines it under "Commands". There is one: the all clear for a wadi.
//
// A command carries an authentication code made with the command key, a secret the
// backend and the gateway share with nobody: not with the wadi nodes and siren nodes,
// whose site key signs radio packets only, and not with the MQTT broker. The gateway
// acts on a command only through readAllClearCommand, which checks the code.
#pragma once

#include <cstddef>
#include <cstdint>

#include "wadiwatch/site_config.h"

namespace wadiwatch {

// The contract allows no longer command. Give a receive buffer one byte more.
constexpr size_t kMaxDownlinkCommandLength = 256;

constexpr size_t kCommandKeySize = 32;

// What commands are checked against. It stays on the gateway: it is not part of the
// site configuration, which every node of the site is built with.
struct CommandAuth {
  // Shared by the backend and the gateway and by nobody else.
  uint8_t key[kCommandKeySize];
};

// The command key the gateway of the prototype is built with: a published test key
// unless a local key file is present (see command_key.cpp).
const CommandAuth& prototypeCommandAuth();

// The published test key itself, whatever the local key file says: the key the
// shared contract examples (docs/gateway-contract/backend-commands.jsonl) are signed with.
const CommandAuth& publishedTestCommandAuth();

// An officer's all clear for one wadi, as the backend issued it.
struct AllClearCommand {
  // The backend's number for the command, never zero.
  uint32_t command;
  // When the backend issued it: Unix seconds, UTC.
  uint32_t issuedAt;
  WadiId wadi;
};

// A command's authentication code as it travels: HMAC-SHA-256 in lower-case
// hexadecimal, and the zero that ends the text.
constexpr size_t kCommandAuthCodeLength = 64;
struct CommandAuthCode {
  char text[kCommandAuthCodeLength + 1];
};

// The authentication code the contract gives a command under a key: HMAC-SHA-256 of
// the command's fields, laid out as the contract says, not of its JSON text.
void writeCommandAuthCode(const CommandAuth& auth,
                          const AllClearCommand& command,
                          CommandAuthCode& code);

// Reads the text of one command: a flat JSON object with exactly the fields of the
// contract, each a whole number in its range or one of the contract's words, and an
// authentication code of the right shape. Returns false, leaving `command` and `code`
// alone, for anything else: text that is not such an object, a missing or repeated
// field, a field the contract does not list, another version or kind.
//
// This says what a command asks for, not who sent it: the code is handed back
// unchecked. Nothing may act on a command read this way.
bool parseAllClearCommand(const char* text,
                          size_t length,
                          AllClearCommand& command,
                          CommandAuthCode& code);

// Reads one command and checks that it comes from the holder of the command key.
// Returns true, and fills in `command`, only for a command that parseAllClearCommand
// reads and whose authentication code is the one `auth` gives for its fields. A
// command that is refused is never acted on in part.
bool readAllClearCommand(const char* text,
                         size_t length,
                         const CommandAuth& auth,
                         AllClearCommand& command);

}  // namespace wadiwatch
