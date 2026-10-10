#include "wadiwatch/sms_templates.h"

#include <cstring>

namespace wadiwatch {

namespace {

// ---------------------------------------------------------------------------
// The wording. This file is UTF-8; keep it so.
//
// Each alert level has a name and a text. In a text, {level} stands for the
// name of the alert level and {wadi} for the name of the wadi (which comes
// from the site configuration). Every text must name both and say what to do.
//
// Length: one SMS carries 160 characters when all of them are in the basic GSM
// alphabet, otherwise 70; a longer text has to be sent in several parts.
// Arabic is always in the 70 case. French stays in the 160 case as long as its
// accented letters are among é è à ù É: ê â î ô û ç œ are not in the basic
// alphabet. The texts below fit one SMS with a wadi name of up to 12
// characters in Arabic; that is why they carry no sender name.
//
// These are starting texts, to be revised after testing with residents.
// ---------------------------------------------------------------------------

struct LevelWording {
  const char* name;
  const char* text;
};

struct Wording {
  LevelWording watch;
  LevelWording warning;
  LevelWording evacuate;
};

const Wording kArabic = {
    /* watch */ {
        "يقظة",
        "{level} - {wadi}: منسوب المياه يرتفع. تابعوا الوضع وجهزوا الفرق.",
    },
    /* warning */ {
        "إنذار",
        "{level} - {wadi}: خطر فيضان. ابتعدوا عن الوادي بمواشيكم وسياراتكم.",
    },
    /* evacuate */ {
        "إخلاء",
        "{level} - {wadi}: الوادي يفيض. غادروا فورا واصعدوا إلى مكان مرتفع.",
    },
};

const Wording kFrench = {
    /* watch */ {
        "VIGILANCE",
        "{level} - {wadi} : le niveau de l'oued monte. Suivez la situation et préparez les "
        "équipes.",
    },
    /* warning */ {
        "ALERTE",
        "{level} - {wadi} : crue possible. Éloignez-vous de l'oued. Mettez à l'abri bétail, "
        "véhicules et pompes.",
    },
    /* evacuate */ {
        "ÉVACUATION",
        "{level} - {wadi} : crue en cours. Quittez tout de suite les abords de l'oued et montez "
        "sur un point haut.",
    },
};

const char kLevelPlaceholder[] = "{level}";
const char kWadiPlaceholder[] = "{wadi}";

// ---------------------------------------------------------------------------
// End of the wording.
// ---------------------------------------------------------------------------

const Wording& wordingIn(Language language) {
  return language == Language::Arabic ? kArabic : kFrench;
}

// The wording for an alert level, or null for no alert.
const LevelWording* wordingFor(Language language, AlertLevel alertLevel) {
  const Wording& wording = wordingIn(language);
  switch (alertLevel) {
    case AlertLevel::Watch:
      return &wording.watch;
    case AlertLevel::Warning:
      return &wording.warning;
    case AlertLevel::Evacuate:
      return &wording.evacuate;
    case AlertLevel::None:
      break;
  }
  return nullptr;
}

// In UTF-8, every byte of a character after its first looks like this.
bool isContinuationByte(char c) { return (static_cast<uint8_t>(c) & 0xC0) == 0x80; }

// Fills a buffer with text, always leaving room for the zero byte at the end.
class TextWriter {
 public:
  TextWriter(char* buffer, size_t capacity) : buffer_(buffer), capacity_(capacity) {}

  void append(const char* text, size_t length) {
    for (size_t i = 0; i < length; ++i) put(text[i]);
  }

  void append(const char* text) { append(text, std::strlen(text)); }

  // Ends the text and returns its length.
  size_t finish() {
    buffer_[length_] = '\0';
    return length_;
  }

 private:
  void put(char c) {
    if (full_) return;
    if (length_ + 1 < capacity_) {
      buffer_[length_++] = c;
      return;
    }
    full_ = true;
    // Do not keep the first bytes of a character whose last bytes did not fit.
    if (!isContinuationByte(c)) return;
    while (length_ > 0 && isContinuationByte(buffer_[length_ - 1])) --length_;
    if (length_ > 0) --length_;
  }

  char* buffer_;
  size_t capacity_;
  size_t length_ = 0;
  bool full_ = false;
};

bool startsWith(const char* text, const char* prefix) {
  return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

}  // namespace

const char* alertLevelName(Language language, AlertLevel alertLevel) {
  const LevelWording* wording = wordingFor(language, alertLevel);
  return wording != nullptr ? wording->name : "";
}

size_t renderAlertSms(Language language, AlertLevel alertLevel, const char* wadiName, char* buffer,
                      size_t capacity) {
  if (capacity == 0) return 0;
  TextWriter writer(buffer, capacity);
  const LevelWording* wording = wordingFor(language, alertLevel);
  if (wording == nullptr) return writer.finish();

  const char* text = wording->text;
  while (*text != '\0') {
    if (startsWith(text, kLevelPlaceholder)) {
      writer.append(wording->name);
      text += std::strlen(kLevelPlaceholder);
    } else if (startsWith(text, kWadiPlaceholder)) {
      writer.append(wadiName);
      text += std::strlen(kWadiPlaceholder);
    } else {
      writer.append(text, 1);
      ++text;
    }
  }
  return writer.finish();
}

}  // namespace wadiwatch
