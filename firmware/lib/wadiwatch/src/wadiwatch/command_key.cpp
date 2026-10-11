#include "wadiwatch/downlink_command.h"

#include <cstring>

// A real command key never goes into the repository. It follows the rules of the site
// key (site_config.cpp): for a real site, put it in the same local file,
// wadiwatch/site_key_local.h (ignored by git), as
//
//   #define WADIWATCH_COMMAND_KEY { 0x.., 0x.., ... }   // kCommandKeySize random bytes
//
// and give the backend the same bytes (WADIWATCH_COMMAND_KEY, see the README). It must
// not be the site key: every wadi node and siren node holds that one. Only the gateway
// firmware refers to this file, so the key is not built into the other devices.
#if defined(__has_include)
#if __has_include("wadiwatch/site_key_local.h")
#include "wadiwatch/site_key_local.h"
#endif
#endif

// TEST KEY, NOT SECRET: the ASCII text "WADIWATCH-TEST-COMMAND-KEY-ONLY!".
// It is published with the source, so anyone can sign commands with it.
// Good for tests and the bench only. The backend has the same one (downlink.py).
#define WADIWATCH_TEST_COMMAND_KEY                                                     \
  {                                                                                    \
    'W', 'A', 'D', 'I', 'W', 'A', 'T', 'C', 'H', '-', 'T', 'E', 'S', 'T', '-', 'C',    \
    'O', 'M', 'M', 'A', 'N', 'D', '-', 'K', 'E', 'Y', '-', 'O', 'N', 'L', 'Y', '!'     \
  }

#ifndef WADIWATCH_COMMAND_KEY
#define WADIWATCH_COMMAND_KEY WADIWATCH_TEST_COMMAND_KEY
#endif

namespace wadiwatch {

namespace {

const uint8_t kCommandKey[] = WADIWATCH_COMMAND_KEY;
static_assert(sizeof(kCommandKey) == kCommandKeySize,
              "the command key must be exactly kCommandKeySize bytes");
const uint8_t kTestCommandKey[] = WADIWATCH_TEST_COMMAND_KEY;
static_assert(sizeof(kTestCommandKey) == kCommandKeySize,
              "the test command key must be exactly kCommandKeySize bytes");

CommandAuth makeCommandAuth(const uint8_t* key) {
  CommandAuth auth;
  std::memcpy(auth.key, key, kCommandKeySize);
  return auth;
}

}  // namespace

const CommandAuth& prototypeCommandAuth() {
  static const CommandAuth auth = makeCommandAuth(kCommandKey);
  return auth;
}

const CommandAuth& publishedTestCommandAuth() {
  static const CommandAuth auth = makeCommandAuth(kTestCommandKey);
  return auth;
}

}  // namespace wadiwatch
