// Direct tests of the SMS templates: pure text, with no devices involved.
// They check what every wording must satisfy, not the wording itself, so that
// the texts can be changed after testing with residents.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include <unity.h>

#include "wadiwatch/recipients.h"
#include "wadiwatch/site_config.h"
#include "wadiwatch/sms_templates.h"

using wadiwatch::AlertLevel;
using wadiwatch::alertLevelName;
using wadiwatch::kMaxSmsTextLength;
using wadiwatch::Language;
using wadiwatch::renderAlertSms;
using wadiwatch::WadiConfig;

void setUp() {}
void tearDown() {}

namespace {

const Language kLanguages[] = {Language::Arabic, Language::French};
const AlertLevel kAlertLevels[] = {AlertLevel::Watch, AlertLevel::Warning, AlertLevel::Evacuate};

const char* wadiNameIn(const WadiConfig& wadi, Language language) {
  return language == Language::Arabic ? wadi.nameArabic : wadi.nameFrench;
}

const WadiConfig& prototypeWadi() { return wadiwatch::prototypeSite().gateway.wadis[0]; }

std::string render(Language language, AlertLevel alertLevel, const char* wadiName) {
  char text[kMaxSmsTextLength + 1];
  const size_t length = renderAlertSms(language, alertLevel, wadiName, text, sizeof(text));
  return std::string(text, length);
}

bool contains(const std::string& text, const char* part) {
  return text.find(part) != std::string::npos;
}

bool hasArabicScript(const std::string& text) {
  for (const char c : text) {
    const uint8_t byte = static_cast<uint8_t>(c);
    if (byte >= 0xD8 && byte <= 0xDB) return true;
  }
  return false;
}

// Well-formed UTF-8 of one to three bytes per character, which covers Arabic and French.
bool isWellFormedUtf8(const std::string& text) {
  size_t i = 0;
  while (i < text.size()) {
    const uint8_t lead = static_cast<uint8_t>(text[i]);
    size_t continuationBytes;
    if (lead < 0x80) {
      continuationBytes = 0;
    } else if (lead >= 0xC2 && lead <= 0xDF) {
      continuationBytes = 1;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
      continuationBytes = 2;
    } else {
      return false;
    }
    if (i + continuationBytes >= text.size()) return false;
    for (size_t k = 1; k <= continuationBytes; ++k) {
      if ((static_cast<uint8_t>(text[i + k]) & 0xC0) != 0x80) return false;
    }
    i += continuationBytes + 1;
  }
  return true;
}

}  // namespace

static void test_every_template_names_the_wadi_and_the_alert_level() {
  for (const Language language : kLanguages) {
    for (const AlertLevel alertLevel : kAlertLevels) {
      const char* wadiName = wadiNameIn(prototypeWadi(), language);

      const std::string text = render(language, alertLevel, wadiName);

      TEST_ASSERT_TRUE(contains(text, wadiName));
      TEST_ASSERT_TRUE(contains(text, alertLevelName(language, alertLevel)));
      // More than the two names: the action to take.
      TEST_ASSERT_TRUE(text.size() >
                       std::strlen(wadiName) + std::strlen(alertLevelName(language, alertLevel)));
      // Nothing of the template's own markup is left in the text.
      TEST_ASSERT_FALSE(contains(text, "{"));
      TEST_ASSERT_FALSE(contains(text, "}"));
    }
  }
}

static void test_each_alert_level_has_its_own_name_and_its_own_text() {
  for (const Language language : kLanguages) {
    const char* wadiName = wadiNameIn(prototypeWadi(), language);
    for (const AlertLevel one : kAlertLevels) {
      TEST_ASSERT_TRUE(std::strlen(alertLevelName(language, one)) > 0);
      for (const AlertLevel other : kAlertLevels) {
        if (one == other) continue;
        TEST_ASSERT_TRUE(std::strcmp(alertLevelName(language, one),
                                     alertLevelName(language, other)) != 0);
        TEST_ASSERT_TRUE(render(language, one, wadiName) != render(language, other, wadiName));
      }
    }
  }
}

static void test_arabic_templates_are_in_arabic_script_and_french_ones_are_not() {
  // A wadi name in Latin letters, so that only the template decides the script.
  const char* wadiName = "W";
  for (const AlertLevel alertLevel : kAlertLevels) {
    TEST_ASSERT_TRUE(hasArabicScript(render(Language::Arabic, alertLevel, wadiName)));
    TEST_ASSERT_TRUE(hasArabicScript(alertLevelName(Language::Arabic, alertLevel)));
    TEST_ASSERT_FALSE(hasArabicScript(render(Language::French, alertLevel, wadiName)));
    TEST_ASSERT_FALSE(hasArabicScript(alertLevelName(Language::French, alertLevel)));
  }
}

static void test_every_template_is_well_formed_utf8() {
  for (const Language language : kLanguages) {
    for (const AlertLevel alertLevel : kAlertLevels) {
      const std::string text =
          render(language, alertLevel, wadiNameIn(prototypeWadi(), language));
      TEST_ASSERT_TRUE(isWellFormedUtf8(text));
    }
  }
}

static void test_every_template_fits_an_sms_with_the_wadi_names_of_the_prototype_site() {
  const wadiwatch::GatewayConfig& gateway = wadiwatch::prototypeSite().gateway;
  for (size_t w = 0; w < gateway.wadiCount; ++w) {
    for (const Language language : kLanguages) {
      for (const AlertLevel alertLevel : kAlertLevels) {
        // Rendered with room to spare, the text is no longer than an SMS may be.
        char roomy[4 * kMaxSmsTextLength];
        const size_t length = renderAlertSms(language, alertLevel,
                                             wadiNameIn(gateway.wadis[w], language), roomy,
                                             sizeof(roomy));
        TEST_ASSERT_TRUE(length > 0);
        TEST_ASSERT_TRUE(length <= kMaxSmsTextLength);
      }
    }
  }
}

static void test_a_text_too_long_for_the_buffer_is_cut_between_characters() {
  for (const Language language : kLanguages) {
    const char* wadiName = wadiNameIn(prototypeWadi(), language);
    const std::string whole = render(language, AlertLevel::Evacuate, wadiName);

    // Every buffer size from too small for anything up to just too small for the whole text.
    for (size_t capacity = 1; capacity <= whole.size(); ++capacity) {
      char guarded[kMaxSmsTextLength + 2];
      std::memset(guarded, '#', sizeof(guarded));

      const size_t length =
          renderAlertSms(language, AlertLevel::Evacuate, wadiName, guarded, capacity);

      TEST_ASSERT_TRUE(length < capacity);
      TEST_ASSERT_EQUAL_CHAR('\0', guarded[length]);
      TEST_ASSERT_EQUAL_CHAR('#', guarded[capacity]);
      const std::string cut(guarded);
      TEST_ASSERT_EQUAL_UINT(length, cut.size());
      TEST_ASSERT_TRUE(isWellFormedUtf8(cut));
      TEST_ASSERT_TRUE(whole.compare(0, cut.size(), cut) == 0);
    }
  }
}

static void test_the_text_is_terminated_and_its_length_returned() {
  for (const Language language : kLanguages) {
    for (const AlertLevel alertLevel : kAlertLevels) {
      char text[kMaxSmsTextLength + 1];
      std::memset(text, '#', sizeof(text));

      const size_t length = renderAlertSms(language, alertLevel,
                                           wadiNameIn(prototypeWadi(), language), text,
                                           sizeof(text));

      TEST_ASSERT_TRUE(length > 0);
      TEST_ASSERT_TRUE(length <= kMaxSmsTextLength);
      TEST_ASSERT_EQUAL_CHAR('\0', text[length]);
      TEST_ASSERT_EQUAL_UINT(length, std::strlen(text));
    }
  }
}

static void test_no_alert_has_no_text() {
  for (const Language language : kLanguages) {
    char text[kMaxSmsTextLength + 1] = "untouched";

    const size_t length = renderAlertSms(language, AlertLevel::None, "W", text, sizeof(text));

    TEST_ASSERT_EQUAL_UINT(0, length);
    TEST_ASSERT_EQUAL_STRING("", text);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_template_names_the_wadi_and_the_alert_level);
  RUN_TEST(test_each_alert_level_has_its_own_name_and_its_own_text);
  RUN_TEST(test_arabic_templates_are_in_arabic_script_and_french_ones_are_not);
  RUN_TEST(test_every_template_is_well_formed_utf8);
  RUN_TEST(test_every_template_fits_an_sms_with_the_wadi_names_of_the_prototype_site);
  RUN_TEST(test_a_text_too_long_for_the_buffer_is_cut_between_characters);
  RUN_TEST(test_the_text_is_terminated_and_its_length_returned);
  RUN_TEST(test_no_alert_has_no_text);
  return UNITY_END();
}
