// Tests of the test harness itself: the in-memory radio and the fake clock.
#include <chrono>

#include <unity.h>

#include "wadiwatch_testing/test_site.h"

using wadiwatch::testing::Air;
using wadiwatch::testing::Frame;
using wadiwatch::testing::InMemoryRadio;
using wadiwatch::testing::TestSite;

void setUp() {}
void tearDown() {}

static Frame receiveFrame(InMemoryRadio& radio) {
  uint8_t buffer[64];
  const size_t length = radio.receive(buffer, sizeof(buffer));
  return Frame(buffer, buffer + length);
}

static void test_radio_delivers_a_frame_to_the_other_radios_but_not_the_sender() {
  Air air;
  InMemoryRadio& a = air.join();
  InMemoryRadio& b = air.join();
  InMemoryRadio& c = air.join();
  const Frame sent = {1, 2, 3};

  a.send(sent.data(), sent.size());

  TEST_ASSERT_TRUE(receiveFrame(b) == sent);
  TEST_ASSERT_TRUE(receiveFrame(c) == sent);
  TEST_ASSERT_TRUE(receiveFrame(a).empty());
}

static void test_radio_drops_the_requested_number_of_frames() {
  Air air;
  InMemoryRadio& a = air.join();
  InMemoryRadio& b = air.join();
  const Frame lost = {1};
  const Frame kept = {2};

  air.dropNext(1);
  a.send(lost.data(), lost.size());
  a.send(kept.data(), kept.size());

  TEST_ASSERT_TRUE(receiveFrame(b) == kept);
  TEST_ASSERT_TRUE(receiveFrame(b).empty());
  TEST_ASSERT_EQUAL_UINT(2, air.transmitted().size());
}

static void test_radio_delivers_an_injected_frame_to_every_radio() {
  Air air;
  InMemoryRadio& a = air.join();
  InMemoryRadio& b = air.join();
  const Frame injected = {9, 9};

  air.inject(injected);

  TEST_ASSERT_TRUE(receiveFrame(a) == injected);
  TEST_ASSERT_TRUE(receiveFrame(b) == injected);
  TEST_ASSERT_EQUAL_UINT(0, air.transmitted().size());
}

static void test_a_simulated_day_runs_in_under_a_second() {
  TestSite site;
  const auto start = std::chrono::steady_clock::now();

  site.runMinutes(24 * 60);

  const auto elapsed = std::chrono::steady_clock::now() - start;
  TEST_ASSERT_TRUE(elapsed < std::chrono::seconds(1));
  TEST_ASSERT_EQUAL_UINT64(24ull * 60 * 60 * 1000, site.clock.nowMs());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_radio_delivers_a_frame_to_the_other_radios_but_not_the_sender);
  RUN_TEST(test_radio_drops_the_requested_number_of_frames);
  RUN_TEST(test_radio_delivers_an_injected_frame_to_every_radio);
  RUN_TEST(test_a_simulated_day_runs_in_under_a_second);
  return UNITY_END();
}
