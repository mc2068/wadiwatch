// Tests of the test harness itself: the in-memory radio, the fake clock and the
// backend's side of the gateway's uplink.
#include <chrono>
#include <string>

#include <unity.h>

#include "wadiwatch_testing/test_site.h"

using wadiwatch::testing::Air;
using wadiwatch::testing::FakeClock;
using wadiwatch::testing::FakeSleepControl;
using wadiwatch::testing::FakeUplink;
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

static void test_radio_with_its_receiver_off_hears_nothing() {
  Air air;
  InMemoryRadio& a = air.join();
  InMemoryRadio& b = air.join();
  const Frame missed = {1};
  const Frame heard = {2};
  TEST_ASSERT_TRUE(b.isListening());

  b.setListening(false);
  a.send(missed.data(), missed.size());
  air.inject(missed);
  b.setListening(true);
  a.send(heard.data(), heard.size());

  // What was sent while the receiver was off is lost, not kept for later.
  TEST_ASSERT_TRUE(receiveFrame(b) == heard);
  TEST_ASSERT_TRUE(receiveFrame(b).empty());
}

static void test_radio_keeps_the_frames_it_heard_when_its_receiver_is_switched_off() {
  Air air;
  InMemoryRadio& a = air.join();
  InMemoryRadio& b = air.join();
  const Frame heard = {7};

  a.send(heard.data(), heard.size());
  b.setListening(false);

  TEST_ASSERT_TRUE(receiveFrame(b) == heard);
}

static void test_a_frame_wakes_a_sleeping_device_only_while_its_receiver_is_on() {
  FakeClock clock;
  FakeSleepControl sleep(clock);
  Air air;
  InMemoryRadio& a = air.join();
  InMemoryRadio& b = air.join(&sleep);
  const Frame frame = {3};

  b.setListening(false);
  sleep.requestSleepMs(1000);
  a.send(frame.data(), frame.size());
  TEST_ASSERT_TRUE(sleep.isAsleep());

  b.setListening(true);
  a.send(frame.data(), frame.size());
  TEST_ASSERT_FALSE(sleep.isAsleep());
}

static void test_each_radio_lists_the_frames_it_sent() {
  Air air;
  InMemoryRadio& a = air.join();
  InMemoryRadio& b = air.join();
  const Frame fromA = {1, 1};
  const Frame fromB = {2};

  air.dropNext(1);
  a.send(fromA.data(), fromA.size());
  b.send(fromB.data(), fromB.size());
  air.inject(fromB);

  // A frame lost on the way still took its time on the air.
  TEST_ASSERT_EQUAL_UINT(1, a.sent().size());
  TEST_ASSERT_TRUE(a.sent()[0] == fromA);
  TEST_ASSERT_EQUAL_UINT(1, b.sent().size());
  TEST_ASSERT_TRUE(b.sent()[0] == fromB);
}

static void test_a_simulated_day_runs_in_under_a_second() {
  TestSite site;
  const auto start = std::chrono::steady_clock::now();

  site.runMinutes(24 * 60);

  const auto elapsed = std::chrono::steady_clock::now() - start;
  TEST_ASSERT_TRUE(elapsed < std::chrono::seconds(1));
  TEST_ASSERT_EQUAL_UINT64(24ull * 60 * 60 * 1000, site.clock.nowMs());
}

static void test_radio_drops_the_requested_number_of_frames_of_one_sender_only() {
  Air air;
  InMemoryRadio& a = air.join();
  InMemoryRadio& b = air.join();
  InMemoryRadio& listener = air.join();
  const Frame fromA = {1};
  const Frame fromB = {2};

  air.dropNextFrom(a, 2);
  a.send(fromA.data(), fromA.size());
  b.send(fromB.data(), fromB.size());
  a.send(fromA.data(), fromA.size());
  a.send(fromA.data(), fromA.size());

  TEST_ASSERT_TRUE(receiveFrame(listener) == fromB);
  TEST_ASSERT_TRUE(receiveFrame(listener) == fromA);
  TEST_ASSERT_TRUE(receiveFrame(listener).empty());
  TEST_ASSERT_EQUAL_UINT(4, air.transmitted().size());
}

// The next command the uplink hands over to a device whose buffer holds `capacity` bytes.
static std::string receiveCommand(FakeUplink& uplink, size_t capacity = 128) {
  std::vector<char> buffer(capacity);
  const size_t length = uplink.receive(buffer.data(), buffer.size());
  return std::string(buffer.data(), length);
}

static void test_the_uplink_hands_over_the_commands_delivered_to_it_in_order() {
  FakeUplink uplink;
  TEST_ASSERT_TRUE(receiveCommand(uplink).empty());

  uplink.deliver("first");
  uplink.deliver("second");

  TEST_ASSERT_EQUAL_STRING("first", receiveCommand(uplink).c_str());
  TEST_ASSERT_EQUAL_STRING("second", receiveCommand(uplink).c_str());
  TEST_ASSERT_TRUE(receiveCommand(uplink).empty());
}

static void test_a_command_delivered_during_an_uplink_outage_comes_when_the_outage_ends() {
  FakeUplink uplink;
  uplink.startOutage();
  uplink.deliver("held at the broker");

  TEST_ASSERT_TRUE(receiveCommand(uplink).empty());

  uplink.endOutage();
  TEST_ASSERT_EQUAL_STRING("held at the broker", receiveCommand(uplink).c_str());
}

static void test_a_command_too_long_for_the_buffer_is_dropped() {
  FakeUplink uplink;
  uplink.deliver(std::string(200, 'x'));
  uplink.deliver("next");

  TEST_ASSERT_TRUE(receiveCommand(uplink).empty());
  TEST_ASSERT_EQUAL_STRING("next", receiveCommand(uplink).c_str());
}

// The command a scenario has the backend send is worded as the contract words it,
// numbered like the backend's (one after the other, from one) and signed with the
// command key the site's gateway holds.
static void test_the_site_issues_all_clear_commands_as_the_backend_does() {
  TestSite site;
  const wadiwatch::WadiId wadi = site.config.gateway.wadis[0].id;
  site.unplugGateway();
  site.runMinutes(1);
  const uint32_t issuedAt = site.wallClock.unixSeconds();

  TEST_ASSERT_EQUAL_UINT32(1, site.issueAllClear());
  TEST_ASSERT_EQUAL_UINT32(2, site.issueAllClear(wadi));

  wadiwatch::CommandAuthCode code;
  wadiwatch::writeCommandAuthCode(site.commandAuth,
                                  wadiwatch::AllClearCommand{1, issuedAt, wadi}, code);
  TEST_ASSERT_EQUAL_STRING(
      ("{\"v\":1,\"kind\":\"all_clear\",\"command\":1,\"ts\":" + std::to_string(issuedAt) +
       ",\"wadi\":" + std::to_string(wadi) + ",\"auth\":\"" + code.text + "\"}")
          .c_str(),
      receiveCommand(site.uplink, wadiwatch::kMaxDownlinkCommandLength + 1).c_str());
  TEST_ASSERT_EQUAL_STRING(
      site.allClearCommand(2, issuedAt, wadi).c_str(),
      receiveCommand(site.uplink, wadiwatch::kMaxDownlinkCommandLength + 1).c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_radio_delivers_a_frame_to_the_other_radios_but_not_the_sender);
  RUN_TEST(test_radio_drops_the_requested_number_of_frames);
  RUN_TEST(test_radio_delivers_an_injected_frame_to_every_radio);
  RUN_TEST(test_radio_with_its_receiver_off_hears_nothing);
  RUN_TEST(test_radio_keeps_the_frames_it_heard_when_its_receiver_is_switched_off);
  RUN_TEST(test_a_frame_wakes_a_sleeping_device_only_while_its_receiver_is_on);
  RUN_TEST(test_each_radio_lists_the_frames_it_sent);
  RUN_TEST(test_a_simulated_day_runs_in_under_a_second);
  RUN_TEST(test_radio_drops_the_requested_number_of_frames_of_one_sender_only);
  RUN_TEST(test_the_uplink_hands_over_the_commands_delivered_to_it_in_order);
  RUN_TEST(test_a_command_delivered_during_an_uplink_outage_comes_when_the_outage_ends);
  RUN_TEST(test_a_command_too_long_for_the_buffer_is_dropped);
  RUN_TEST(test_the_site_issues_all_clear_commands_as_the_backend_does);
  return UNITY_END();
}
