// Direct tests of the packet codec: the six packet kinds, the size on the air
// and the edge cases of the message counter.
#include <cstdint>

#include <unity.h>

#include "wadiwatch/packet.h"
#include "wadiwatch/site_config.h"
#include "wadiwatch_testing/fakes.h"

using wadiwatch::AlertLevel;
using wadiwatch::NodeId;
using wadiwatch::Packet;
using wadiwatch::PacketCodec;
using wadiwatch::PacketKind;
using wadiwatch::SiteAuth;
using wadiwatch::StorageRecord;
using wadiwatch::kMaxPacketSize;
using wadiwatch::kPacketSize;
using wadiwatch::storageKey;
using wadiwatch::testing::FakeStorage;
using wadiwatch::testing::Frame;

void setUp() {}
void tearDown() {}

// The prototype site with one more node, so that a receiver can hear two senders.
static SiteAuth siteWithThreeNodes() {
  SiteAuth auth = wadiwatch::prototypeSite().auth;
  NodeId highest = 0;
  for (size_t i = 0; i < auth.nodeCount; ++i) {
    if (auth.nodes[i] > highest) highest = auth.nodes[i];
  }
  auth.nodes[auth.nodeCount++] = static_cast<NodeId>(highest + 1);
  return auth;
}

// A sender, a second sender and a receiver of one site, each with its own storage.
struct Link {
  SiteAuth auth = siteWithThreeNodes();
  NodeId senderId = auth.nodes[0];
  NodeId receiverId = auth.nodes[1];
  NodeId otherSenderId = auth.nodes[2];
  FakeStorage senderStorage;
  FakeStorage receiverStorage;
  FakeStorage otherSenderStorage;
  PacketCodec sender{auth, senderId, senderStorage};
  PacketCodec receiver{auth, receiverId, receiverStorage};
  PacketCodec otherSender{auth, otherSenderId, otherSenderStorage};
};

static Packet testPacket() {
  Packet packet = Packet();
  packet.kind = PacketKind::Test;
  return packet;
}

static Frame encode(PacketCodec& codec, Packet packet) {
  uint8_t buffer[kMaxPacketSize];
  const size_t length = codec.encode(packet, buffer, sizeof(buffer));
  return Frame(buffer, buffer + length);
}

static bool accepts(PacketCodec& codec, const Frame& frame) {
  Packet packet;
  return codec.decode(frame.data(), frame.size(), packet);
}

// Encodes on the sender, decodes on the receiver, and checks what every kind has in common.
static Packet sendAcross(Link& link, Packet sent) {
  uint8_t buffer[kMaxPacketSize];
  const size_t length = link.sender.encode(sent, buffer, sizeof(buffer));
  // The spec asks for about 24 bytes on the air, authentication code included.
  TEST_ASSERT_EQUAL_UINT(24, length);
  TEST_ASSERT_EQUAL_UINT(kPacketSize, length);

  Packet received = Packet();
  TEST_ASSERT_TRUE(link.receiver.decode(buffer, length, received));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(sent.kind), static_cast<uint8_t>(received.kind));
  TEST_ASSERT_EQUAL_UINT16(link.senderId, received.sender);
  TEST_ASSERT_EQUAL_UINT32(sent.counter, received.counter);
  return received;
}

static void test_reading_round_trips() {
  Link link;
  Packet sent = Packet();
  sent.kind = PacketKind::Reading;
  sent.reading.levelMm = 1234;
  sent.reading.rateOfRiseMmPerMin = -57;
  sent.reading.rainfallTenthsMm = 105;
  sent.reading.alertLevel = AlertLevel::Watch;
  sent.reading.stormMode = true;

  const Packet received = sendAcross(link, sent);

  TEST_ASSERT_EQUAL_UINT16(1234, received.reading.levelMm);
  TEST_ASSERT_EQUAL_INT16(-57, received.reading.rateOfRiseMmPerMin);
  TEST_ASSERT_EQUAL_UINT16(105, received.reading.rainfallTenthsMm);
  TEST_ASSERT_TRUE(received.reading.alertLevel == AlertLevel::Watch);
  TEST_ASSERT_TRUE(received.reading.stormMode);
  TEST_ASSERT_FALSE(received.reading.levelRepeated);

  // A level the node could not measure again travels marked as repeated, and the
  // two flags travel separately.
  sent.reading.stormMode = false;
  sent.reading.levelRepeated = true;
  const Packet repeated = sendAcross(link, sent);
  TEST_ASSERT_FALSE(repeated.reading.stormMode);
  TEST_ASSERT_TRUE(repeated.reading.levelRepeated);
  TEST_ASSERT_EQUAL_UINT16(1234, repeated.reading.levelMm);
}

static void test_alert_round_trips() {
  Link link;
  Packet sent = Packet();
  sent.kind = PacketKind::Alert;
  sent.alert.alertLevel = AlertLevel::Evacuate;
  sent.alert.levelMm = 2250;
  sent.alert.rateOfRiseMmPerMin = 50;

  const Packet received = sendAcross(link, sent);

  TEST_ASSERT_TRUE(received.alert.alertLevel == AlertLevel::Evacuate);
  TEST_ASSERT_EQUAL_UINT16(2250, received.alert.levelMm);
  TEST_ASSERT_EQUAL_INT16(50, received.alert.rateOfRiseMmPerMin);
  TEST_ASSERT_FALSE(received.alert.refresh);

  // The same alert sent again a minute later says that it is a refresh.
  sent.alert.refresh = true;
  const Packet refresh = sendAcross(link, sent);
  TEST_ASSERT_TRUE(refresh.alert.refresh);
  TEST_ASSERT_TRUE(refresh.alert.alertLevel == AlertLevel::Evacuate);
  TEST_ASSERT_FALSE(refresh.alert.levelRepeated);

  // And one sent while the node cannot measure says that its level is repeated.
  sent.alert.refresh = false;
  sent.alert.levelRepeated = true;
  const Packet repeated = sendAcross(link, sent);
  TEST_ASSERT_FALSE(repeated.alert.refresh);
  TEST_ASSERT_TRUE(repeated.alert.levelRepeated);
  TEST_ASSERT_EQUAL_UINT16(2250, repeated.alert.levelMm);
}

static void test_acknowledgement_round_trips() {
  Link link;
  Packet sent = Packet();
  sent.kind = PacketKind::Acknowledgement;
  sent.acknowledgement.alertSender = link.otherSenderId;
  sent.acknowledgement.alertCounter = 0x01020304;
  sent.acknowledgement.alertLevel = AlertLevel::Warning;

  const Packet received = sendAcross(link, sent);

  TEST_ASSERT_EQUAL_UINT16(link.otherSenderId, received.acknowledgement.alertSender);
  TEST_ASSERT_EQUAL_UINT32(0x01020304, received.acknowledgement.alertCounter);
  TEST_ASSERT_TRUE(received.acknowledgement.alertLevel == AlertLevel::Warning);
}

static void test_heartbeat_round_trips() {
  Link link;
  Packet sent = Packet();
  sent.kind = PacketKind::Heartbeat;
  sent.heartbeat.batteryMillivolts = 3712;
  sent.heartbeat.signalStrengthDbm = -131;
  sent.heartbeat.lastLevelMm = 420;
  sent.heartbeat.discardedReadings = 7;
  sent.heartbeat.tamper = true;
  sent.heartbeat.hasLastLevel = false;
  sent.heartbeat.alertRefreshLost = false;

  Packet received = sendAcross(link, sent);

  TEST_ASSERT_EQUAL_UINT16(3712, received.heartbeat.batteryMillivolts);
  TEST_ASSERT_EQUAL_INT16(-131, received.heartbeat.signalStrengthDbm);
  TEST_ASSERT_EQUAL_UINT16(420, received.heartbeat.lastLevelMm);
  TEST_ASSERT_EQUAL_UINT8(7, received.heartbeat.discardedReadings);
  TEST_ASSERT_TRUE(received.heartbeat.tamper);
  TEST_ASSERT_FALSE(received.heartbeat.hasLastLevel);
  TEST_ASSERT_FALSE(received.heartbeat.alertRefreshLost);

  // The three flags travel separately.
  sent.heartbeat.tamper = false;
  sent.heartbeat.alertRefreshLost = true;
  received = sendAcross(link, sent);
  TEST_ASSERT_FALSE(received.heartbeat.tamper);
  TEST_ASSERT_FALSE(received.heartbeat.hasLastLevel);
  TEST_ASSERT_TRUE(received.heartbeat.alertRefreshLost);

  sent.heartbeat.alertRefreshLost = false;
  sent.heartbeat.hasLastLevel = true;
  received = sendAcross(link, sent);
  TEST_ASSERT_FALSE(received.heartbeat.tamper);
  TEST_ASSERT_TRUE(received.heartbeat.hasLastLevel);
  TEST_ASSERT_FALSE(received.heartbeat.alertRefreshLost);
  TEST_ASSERT_FALSE(received.heartbeat.lastLevelRepeated);

  sent.heartbeat.lastLevelRepeated = true;
  received = sendAcross(link, sent);
  TEST_ASSERT_TRUE(received.heartbeat.hasLastLevel);
  TEST_ASSERT_TRUE(received.heartbeat.lastLevelRepeated);
  TEST_ASSERT_FALSE(received.heartbeat.tamper);
  TEST_ASSERT_FALSE(received.heartbeat.alertRefreshLost);
}

static void test_all_clear_round_trips() {
  Link link;
  Packet sent = Packet();
  sent.kind = PacketKind::AllClear;
  sent.allClear.wadiNode = link.otherSenderId;
  sent.allClear.command = 0xFFFFFFFEu;

  const Packet received = sendAcross(link, sent);

  TEST_ASSERT_EQUAL_UINT16(link.otherSenderId, received.allClear.wadiNode);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFEu, received.allClear.command);
}

static void test_test_packet_round_trips() {
  Link link;

  sendAcross(link, testPacket());
}

// Pins the format on the air. The expected frame was computed outside this code base
// (Python's struct and hmac modules) from the layout documented in packet.cpp.
// If this fails, nodes flashed before and after the change no longer understand each other.
static void test_an_encoded_frame_matches_the_documented_layout() {
  SiteAuth auth = SiteAuth();
  for (size_t i = 0; i < wadiwatch::kSiteKeySize; ++i) auth.key[i] = static_cast<uint8_t>(0x40 + i);
  const NodeId sender = 0x0A0B;
  FakeStorage storage;
  storage.store(storageKey(StorageRecord::SendCounter, sender), 0x01020303);
  PacketCodec codec(auth, sender, storage);
  Packet packet = Packet();
  packet.kind = PacketKind::Alert;
  packet.alert.alertLevel = AlertLevel::Evacuate;
  packet.alert.levelMm = 0x1122;
  packet.alert.rateOfRiseMmPerMin = 0x0033;

  const Frame frame = encode(codec, packet);

  const Frame expected = {
      0x02,                                            // protocol version
      0x02,                                            // kind: alert
      0x0A, 0x0B,                                      // sender
      0x01, 0x02, 0x03, 0x04,                          // counter
      0x03, 0x11, 0x22, 0x00, 0x33, 0x00, 0x00, 0x00,  // alert level, level, rate of rise
      0x50, 0xF5, 0x47, 0x6B, 0xB3, 0xDC, 0x24, 0xED,  // authentication code
  };
  TEST_ASSERT_EQUAL_UINT(expected.size(), frame.size());
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected.data(), frame.data(), expected.size());
}

static void test_counters_start_at_one_and_rise_by_one() {
  Link link;
  Packet first = testPacket();
  Packet second = testPacket();
  uint8_t buffer[kMaxPacketSize];

  link.sender.encode(first, buffer, sizeof(buffer));
  link.sender.encode(second, buffer, sizeof(buffer));

  TEST_ASSERT_EQUAL_UINT32(1, first.counter);
  TEST_ASSERT_EQUAL_UINT32(2, second.counter);
}

static void test_a_frame_is_accepted_only_once() {
  Link link;
  const Frame frame = encode(link.sender, testPacket());

  TEST_ASSERT_TRUE(accepts(link.receiver, frame));
  TEST_ASSERT_FALSE(accepts(link.receiver, frame));
}

static void test_a_frame_older_than_the_last_accepted_one_is_rejected() {
  Link link;
  const Frame older = encode(link.sender, testPacket());
  const Frame newer = encode(link.sender, testPacket());

  TEST_ASSERT_TRUE(accepts(link.receiver, newer));
  TEST_ASSERT_FALSE(accepts(link.receiver, older));
}

static void test_a_frame_is_accepted_after_earlier_ones_were_lost() {
  Link link;
  encode(link.sender, testPacket());
  encode(link.sender, testPacket());
  const Frame third = encode(link.sender, testPacket());

  TEST_ASSERT_TRUE(accepts(link.receiver, third));
}

static void test_each_sender_has_its_own_counter_at_the_receiver() {
  Link link;
  encode(link.sender, testPacket());
  const Frame fromSender = encode(link.sender, testPacket());
  const Frame fromOtherSender = encode(link.otherSender, testPacket());

  TEST_ASSERT_TRUE(accepts(link.receiver, fromSender));
  // The other sender's counter is lower, and that is fine.
  TEST_ASSERT_TRUE(accepts(link.receiver, fromOtherSender));
}

static void test_frames_of_the_wrong_length_are_rejected() {
  Link link;
  const Frame frame = encode(link.sender, testPacket());
  Frame padded = frame;
  padded.push_back(0);
  const Frame cases[] = {
      Frame(),
      Frame(frame.begin(), frame.end() - 1),
      padded,
  };

  for (const Frame& wrongLength : cases) {
    TEST_ASSERT_FALSE(accepts(link.receiver, wrongLength));
  }
  TEST_ASSERT_TRUE(accepts(link.receiver, frame));
}

static void test_a_node_rejects_a_frame_carrying_its_own_identifier() {
  Link link;
  const Frame frame = encode(link.sender, testPacket());
  FakeStorage storage;
  PacketCodec sameNode(link.auth, link.senderId, storage);

  TEST_ASSERT_FALSE(accepts(sameNode, frame));
}

static void test_a_restarted_sender_carries_on_from_its_stored_counter() {
  Link link;
  encode(link.sender, testPacket());
  const Frame lastBeforeRestart = encode(link.sender, testPacket());
  TEST_ASSERT_TRUE(accepts(link.receiver, lastBeforeRestart));

  PacketCodec restarted(link.auth, link.senderId, link.senderStorage);
  Packet packet = testPacket();
  uint8_t buffer[kMaxPacketSize];
  const size_t length = restarted.encode(packet, buffer, sizeof(buffer));

  TEST_ASSERT_EQUAL_UINT32(3, packet.counter);
  TEST_ASSERT_TRUE(accepts(link.receiver, Frame(buffer, buffer + length)));
}

static void test_a_restarted_receiver_still_rejects_what_it_accepted_before() {
  Link link;
  const Frame frame = encode(link.sender, testPacket());
  TEST_ASSERT_TRUE(accepts(link.receiver, frame));

  PacketCodec restarted(link.auth, link.receiverId, link.receiverStorage);

  TEST_ASSERT_FALSE(accepts(restarted, frame));
  TEST_ASSERT_TRUE(accepts(restarted, encode(link.sender, testPacket())));
}

static void test_the_counter_never_wraps_around() {
  Link link;
  link.senderStorage.store(storageKey(StorageRecord::SendCounter, link.senderId), UINT32_MAX - 1);
  Packet last = testPacket();
  Packet oneTooMany = testPacket();
  uint8_t buffer[kMaxPacketSize];

  TEST_ASSERT_EQUAL_UINT(kPacketSize, link.sender.encode(last, buffer, sizeof(buffer)));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, last.counter);
  TEST_ASSERT_TRUE(link.receiver.decode(buffer, kPacketSize, last));

  // A wrapped counter would make every later packet look like a replay, or let old ones back in.
  TEST_ASSERT_EQUAL_UINT(0, link.sender.encode(oneTooMany, buffer, sizeof(buffer)));
}

static void test_a_buffer_that_is_too_small_gets_nothing_and_uses_no_counter() {
  Link link;
  Packet packet = testPacket();
  uint8_t buffer[kMaxPacketSize];

  TEST_ASSERT_EQUAL_UINT(0, link.sender.encode(packet, buffer, kPacketSize - 1));
  TEST_ASSERT_EQUAL_UINT(kPacketSize, link.sender.encode(packet, buffer, kPacketSize));
  TEST_ASSERT_EQUAL_UINT32(1, packet.counter);
}

static void test_packets_that_make_no_sense_are_not_encoded() {
  Link link;
  uint8_t buffer[kMaxPacketSize];
  Packet unknownKind = Packet();
  unknownKind.kind = static_cast<PacketKind>(0);
  Packet kindPastTheLast = Packet();
  kindPastTheLast.kind = static_cast<PacketKind>(static_cast<uint8_t>(PacketKind::Test) + 1);
  Packet alertPastEvacuate = Packet();
  alertPastEvacuate.kind = PacketKind::Alert;
  alertPastEvacuate.alert.alertLevel =
      static_cast<AlertLevel>(static_cast<uint8_t>(AlertLevel::Evacuate) + 1);
  Packet* const cases[] = {&unknownKind, &kindPastTheLast, &alertPastEvacuate};

  for (Packet* packet : cases) {
    TEST_ASSERT_EQUAL_UINT(0, link.sender.encode(*packet, buffer, sizeof(buffer)));
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_reading_round_trips);
  RUN_TEST(test_alert_round_trips);
  RUN_TEST(test_acknowledgement_round_trips);
  RUN_TEST(test_heartbeat_round_trips);
  RUN_TEST(test_all_clear_round_trips);
  RUN_TEST(test_test_packet_round_trips);
  RUN_TEST(test_an_encoded_frame_matches_the_documented_layout);
  RUN_TEST(test_counters_start_at_one_and_rise_by_one);
  RUN_TEST(test_a_frame_is_accepted_only_once);
  RUN_TEST(test_a_frame_older_than_the_last_accepted_one_is_rejected);
  RUN_TEST(test_a_frame_is_accepted_after_earlier_ones_were_lost);
  RUN_TEST(test_each_sender_has_its_own_counter_at_the_receiver);
  RUN_TEST(test_frames_of_the_wrong_length_are_rejected);
  RUN_TEST(test_a_node_rejects_a_frame_carrying_its_own_identifier);
  RUN_TEST(test_a_restarted_sender_carries_on_from_its_stored_counter);
  RUN_TEST(test_a_restarted_receiver_still_rejects_what_it_accepted_before);
  RUN_TEST(test_the_counter_never_wraps_around);
  RUN_TEST(test_a_buffer_that_is_too_small_gets_nothing_and_uses_no_counter);
  RUN_TEST(test_packets_that_make_no_sense_are_not_encoded);
  return UNITY_END();
}
