#include "wadiwatch/recipients.h"

namespace wadiwatch {

// FAKE NUMBERS ONLY. Every number below is a placeholder that belongs to
// nobody: it starts with +216 00, and Tunisian subscriber numbers never begin
// with 0. A host test fails if a number without that prefix is committed.
//
// Real recipients consent to be registered and their numbers are personal
// data: they are local configuration of the gateway, never in the repository.
const RecipientList& prototypeRecipients() {
  // The wadi of the bench prototype, and a wadi the prototype does not
  // monitor: residents registered near it hear of Evacuate only.
  static const WadiId wadi = prototypeSite().gateway.wadis[0].id;
  static const WadiId anotherWadi = static_cast<WadiId>(wadi + 1);

  static const RecipientList list = {{
      // Fake officials.
      {"+21600000001", Language::French, wadi, RecipientGroup::Official},
      {"+21600000002", Language::Arabic, wadi, RecipientGroup::Official},
      // Fake residents registered near the prototype's wadi.
      {"+21600000011", Language::Arabic, wadi, RecipientGroup::Resident},
      {"+21600000012", Language::French, wadi, RecipientGroup::Resident},
      // Fake resident registered near another wadi.
      {"+21600000021", Language::Arabic, anotherWadi, RecipientGroup::Resident},
  }};
  return list;
}

}  // namespace wadiwatch
