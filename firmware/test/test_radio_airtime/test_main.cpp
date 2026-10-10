// Direct tests of the LoRa time-on-air calculation, against values the Semtech
// LoRa calculator and the published LoRaWAN airtime tables give.
#include <unity.h>

#include "wadiwatch/packet.h"
#include "wadiwatch/radio_config.h"
#include "wadiwatch/site_config.h"

using wadiwatch::loraTimeOnAirUs;
using wadiwatch::RadioConfig;

void setUp() {}
void tearDown() {}

namespace {

// The settings the reference values are published for: 125 kHz, coding rate 4/5,
// a preamble of 8 symbols, explicit header and CRC.
RadioConfig referenceSettings(uint8_t spreadingFactor) {
  RadioConfig radio = wadiwatch::prototypeSite().radio;
  radio.spreadingFactor = spreadingFactor;
  radio.bandwidthHz = 125000;
  radio.codingRateDenominator = 5;
  radio.preambleSymbols = 8;
  return radio;
}

}  // namespace

static void test_time_on_air_of_a_short_frame_at_the_fastest_and_slowest_spreading_factor() {
  TEST_ASSERT_EQUAL_UINT32(41216, loraTimeOnAirUs(referenceSettings(7), 10));
  // From SF11 up at 125 kHz the low data rate optimisation applies.
  TEST_ASSERT_EQUAL_UINT32(991232, loraTimeOnAirUs(referenceSettings(12), 10));
}

static void test_time_on_air_of_the_largest_lorawan_frame_at_each_end_of_the_range() {
  // 51 bytes of application payload make a 64-byte frame: 118.0 ms at SF7, 2793.5 ms at SF12.
  TEST_ASSERT_EQUAL_UINT32(118016, loraTimeOnAirUs(referenceSettings(7), 64));
  TEST_ASSERT_EQUAL_UINT32(2793472, loraTimeOnAirUs(referenceSettings(12), 64));
}

static void test_each_step_of_spreading_factor_makes_a_packet_take_longer() {
  uint32_t previousUs = 0;
  for (uint8_t spreadingFactor = 7; spreadingFactor <= 12; ++spreadingFactor) {
    const uint32_t us = loraTimeOnAirUs(referenceSettings(spreadingFactor), wadiwatch::kPacketSize);
    TEST_ASSERT_TRUE(us > previousUs);
    previousUs = us;
  }
}

static void test_a_wider_band_makes_a_packet_shorter_and_more_coding_makes_it_longer() {
  const RadioConfig reference = referenceSettings(9);
  RadioConfig wider = reference;
  wider.bandwidthHz = 250000;
  RadioConfig moreCoding = reference;
  moreCoding.codingRateDenominator = 8;
  RadioConfig longerPreamble = reference;
  longerPreamble.preambleSymbols = 12;
  const uint32_t referenceUs = loraTimeOnAirUs(reference, wadiwatch::kPacketSize);

  // SF9 at 125 kHz: 205.8 ms for a WadiWatch packet.
  TEST_ASSERT_EQUAL_UINT32(205824, referenceUs);
  TEST_ASSERT_EQUAL_UINT32(referenceUs / 2, loraTimeOnAirUs(wider, wadiwatch::kPacketSize));
  TEST_ASSERT_TRUE(loraTimeOnAirUs(moreCoding, wadiwatch::kPacketSize) > referenceUs);
  // Four more preamble symbols of 4.096 ms each.
  TEST_ASSERT_EQUAL_UINT32(referenceUs + 4 * 4096,
                           loraTimeOnAirUs(longerPreamble, wadiwatch::kPacketSize));
}

static void test_settings_no_lora_radio_accepts_have_no_time_on_air() {
  RadioConfig noBand = referenceSettings(9);
  noBand.bandwidthHz = 0;
  RadioConfig noSpreading = referenceSettings(9);
  noSpreading.spreadingFactor = 0;
  RadioConfig noCoding = referenceSettings(9);
  noCoding.codingRateDenominator = 4;

  TEST_ASSERT_EQUAL_UINT32(0, loraTimeOnAirUs(noBand, wadiwatch::kPacketSize));
  TEST_ASSERT_EQUAL_UINT32(0, loraTimeOnAirUs(noSpreading, wadiwatch::kPacketSize));
  TEST_ASSERT_EQUAL_UINT32(0, loraTimeOnAirUs(noCoding, wadiwatch::kPacketSize));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_time_on_air_of_a_short_frame_at_the_fastest_and_slowest_spreading_factor);
  RUN_TEST(test_time_on_air_of_the_largest_lorawan_frame_at_each_end_of_the_range);
  RUN_TEST(test_each_step_of_spreading_factor_makes_a_packet_take_longer);
  RUN_TEST(test_a_wider_band_makes_a_packet_shorter_and_more_coding_makes_it_longer);
  RUN_TEST(test_settings_no_lora_radio_accepts_have_no_time_on_air);
  return UNITY_END();
}
