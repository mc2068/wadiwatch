// SMS templates: every text a registered recipient reads, in each language.
// All wording lives in sms_templates.cpp and nowhere else.
#pragma once

#include <cstddef>
#include <cstdint>

#include "wadiwatch/packet.h"

namespace wadiwatch {

// The languages a recipient can register for.
enum class Language : uint8_t { Arabic = 0, French = 1 };

// Longest SMS text the gateway builds, in bytes of UTF-8, not counting the
// zero byte that ends it.
constexpr size_t kMaxSmsTextLength = 320;

// What an alert level is called in SMS, in `language`. Empty for no alert.
const char* alertLevelName(Language language, AlertLevel alertLevel);

// Writes the SMS that tells a recipient the wadi called `wadiName` has reached
// `alertLevel`: it names the wadi, the alert level and the action to take.
// The text is UTF-8 and ends with a zero byte. Returns its length in bytes,
// not counting that byte; 0 for no alert. A text too long for the buffer is
// cut short between two characters rather than dropped.
size_t renderAlertSms(Language language, AlertLevel alertLevel, const char* wadiName, char* buffer,
                      size_t capacity);

// What the all clear is called in SMS, in `language`.
const char* allClearName(Language language);

// Writes the closing SMS: it tells a recipient that the event at the wadi called
// `wadiName` is over. Same rules as renderAlertSms for the buffer and the length.
size_t renderClosingSms(Language language, const char* wadiName, char* buffer, size_t capacity);

}  // namespace wadiwatch
