// Registered recipients: the people the gateway sends SMS to.
//
// The list is the gateway's own. It is deliberately not part of the site
// configuration that every device carries, so that no phone number leaves the
// municipal building inside a wadi node or a siren node.
#pragma once

#include <cstddef>
#include <cstdint>

#include "wadiwatch/site_config.h"
#include "wadiwatch/sms_templates.h"

namespace wadiwatch {

enum class RecipientGroup : uint8_t {
  // Municipality and Civil Protection: told from Watch upwards, for every wadi.
  Official = 0,
  // Households, farmers, schools, factories: told at Warning for the wadi they
  // registered near, and at Evacuate for any wadi.
  Resident = 1,
};

// "+" and up to 15 digits (international format).
constexpr size_t kMaxPhoneNumberLength = 16;
constexpr size_t kMaxRecipients = 64;

// Everything that is stored about a registered recipient. Personal data is
// kept to this minimum on purpose: no name, no address. Do not add fields.
struct Recipient {
  // International format (+216...), ending with a zero byte.
  char phoneNumber[kMaxPhoneNumberLength + 1];
  Language language;
  WadiId nearestWadi;
  RecipientGroup group;
};

static_assert(sizeof(Recipient) == kMaxPhoneNumberLength + 1 + 3,
              "a recipient is a phone number, a language, a nearest wadi and a group, nothing more");

struct RecipientList {
  // A slot with an empty phone number is unused.
  Recipient recipients[kMaxRecipients];
};

// The list committed with the prototype. It holds fake numbers only: real
// recipients are local configuration of the gateway and are never committed.
const RecipientList& prototypeRecipients();

}  // namespace wadiwatch
