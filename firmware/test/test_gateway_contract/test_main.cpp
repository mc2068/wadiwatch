// The shared contract check, gateway side.
//
// docs/gateway-contract/gateway-output.jsonl holds what the gateway core hands to its
// uplink for a fixed set of packets covering every message kind. This test produces
// those messages again and fails if the file differs by a byte; the backend tests
// ingest the same file and fail if the backend refuses a line. So the gateway and the
// backend cannot drift apart without a test failing on one side.
//
// After an intended change to the messages, rewrite the file and then run the backend
// tests:
//
//   WADIWATCH_UPDATE_CONTRACT=1 pio test -e native -f test_gateway_contract
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include <unity.h>

#include "wadiwatch/packet.h"
#include "wadiwatch/site_config.h"
#include "wadiwatch/uplink_message.h"
#include "wadiwatch_testing/quiet_site.h"

using wadiwatch::AlertLevel;
using wadiwatch::GatewayConfig;
using wadiwatch::NodeId;
using wadiwatch::Packet;
using wadiwatch::PacketCodec;
using wadiwatch::PacketKind;
using wadiwatch::testing::FakeStorage;
using wadiwatch::testing::QuietSite;

void setUp() {}
void tearDown() {}

namespace {

const char kOutputFile[] = "gateway-output.jsonl";
// A file that is always in the directory of the contract examples.
const char kLandmarkFile[] = "valid.jsonl";
const char kDirectoryVariable[] = "WADIWATCH_CONTRACT_DIR";
const char kUpdateVariable[] = "WADIWATCH_UPDATE_CONTRACT";

uint16_t lowBatteryMillivolts(const GatewayConfig& gateway, NodeId node) {
  for (size_t i = 0; i < gateway.batteryCount; ++i) {
    if (gateway.batteries[i].node == node) return gateway.batteries[i].lowBatteryMillivolts;
  }
  TEST_FAIL_MESSAGE("the site configuration has no low-battery threshold for the node");
  return 0;
}

Packet reading(uint16_t levelMm, int16_t rateOfRiseMmPerMin, uint16_t rainfallTenthsMm,
               AlertLevel alertLevel, bool stormMode) {
  Packet packet = Packet();
  packet.kind = PacketKind::Reading;
  packet.reading.levelMm = levelMm;
  packet.reading.rateOfRiseMmPerMin = rateOfRiseMmPerMin;
  packet.reading.rainfallTenthsMm = rainfallTenthsMm;
  packet.reading.alertLevel = alertLevel;
  packet.reading.stormMode = stormMode;
  return packet;
}

Packet alert(AlertLevel alertLevel, uint16_t levelMm, int16_t rateOfRiseMmPerMin) {
  Packet packet = Packet();
  packet.kind = PacketKind::Alert;
  packet.alert.alertLevel = alertLevel;
  packet.alert.levelMm = levelMm;
  packet.alert.rateOfRiseMmPerMin = rateOfRiseMmPerMin;
  return packet;
}

Packet acknowledgement(const Packet& acknowledged) {
  Packet packet = Packet();
  packet.kind = PacketKind::Acknowledgement;
  packet.acknowledgement.alertSender = acknowledged.sender;
  packet.acknowledgement.alertCounter = acknowledged.counter;
  packet.acknowledgement.alertLevel = acknowledged.alert.alertLevel;
  return packet;
}

// One flood on the prototype site as the gateway hears it, packet by packet, and the
// messages its uplink takes. The packets come from test radios, so the result does
// not change when the wadi nodes or the siren node change what they send.
std::vector<std::string> gatewayOutput() {
  QuietSite site;
  const GatewayConfig& gateway = site.config.gateway;
  auto& wadiNode = site.wadiNodeOnAir;
  auto& downstreamWadiNode = site.downstreamWadiNodeOnAir;
  auto& sirenNode = site.sirenNodeOnAir;

  // A dry wadi, then rain and a rising level.
  site.transmit(wadiNode, reading(80, 0, 0, AlertLevel::None, false));
  site.runMinutes(15);
  site.transmit(wadiNode, reading(60, -4, 46, AlertLevel::None, true));
  site.runMinutes(1);
  site.transmit(wadiNode, reading(845, 32, 124, AlertLevel::Watch, true));
  site.transmit(wadiNode, alert(AlertLevel::Watch, 845, 32));
  site.runMinutes(1);

  // Warning at the upstream wadi node. The siren node acknowledges every alert it accepts.
  const Packet warning = site.transmit(wadiNode, alert(AlertLevel::Warning, 1040, 51));
  site.transmit(sirenNode, acknowledgement(warning));
  site.runMinutes(1);

  // The flood reaches the second wadi node: Warning there too, and its reading
  // behind the alert.
  const Packet downstreamWarning =
      site.transmit(downstreamWadiNode, alert(AlertLevel::Warning, 1015, 53));
  site.transmit(sirenNode, acknowledgement(downstreamWarning));
  site.transmit(downstreamWadiNode, reading(1015, 53, 131, AlertLevel::Warning, true));
  site.runMinutes(1);

  // A wadi node at Warning sends its alert again every minute: a refresh, with a new
  // counter and the level of the moment. The downstream wadi node hears it and raises
  // Evacuate on a level that is still short of that threshold.
  const Packet refresh = site.transmit(wadiNode, alert(AlertLevel::Warning, 1105, 50));
  site.transmit(sirenNode, acknowledgement(refresh));
  const Packet confirmed =
      site.transmit(downstreamWadiNode, alert(AlertLevel::Evacuate, 1090, 51));
  site.transmit(sirenNode, acknowledgement(confirmed));
  site.runMinutes(1);

  // Evacuate at the upstream wadi node, by its own level.
  const Packet evacuate = site.transmit(wadiNode, alert(AlertLevel::Evacuate, 1520, 48));
  site.transmit(sirenNode, acknowledgement(evacuate));
  site.runMinutes(1);

  // A healthy wadi node; a second one low on battery; a siren node low on battery,
  // opened, that lost the refresh. Each battery is judged by its own node's threshold.
  Packet wadiNodeHeartbeat = Packet();
  wadiNodeHeartbeat.kind = PacketKind::Heartbeat;
  wadiNodeHeartbeat.heartbeat.batteryMillivolts =
      static_cast<uint16_t>(lowBatteryMillivolts(gateway, wadiNode.id) + 420);
  wadiNodeHeartbeat.heartbeat.signalStrengthDbm = -97;
  wadiNodeHeartbeat.heartbeat.hasLastLevel = true;
  wadiNodeHeartbeat.heartbeat.lastLevelMm = 1520;
  wadiNodeHeartbeat.heartbeat.discardedReadings = 2;
  site.transmit(wadiNode, wadiNodeHeartbeat);

  Packet downstreamHeartbeat = Packet();
  downstreamHeartbeat.kind = PacketKind::Heartbeat;
  downstreamHeartbeat.heartbeat.batteryMillivolts =
      static_cast<uint16_t>(lowBatteryMillivolts(gateway, downstreamWadiNode.id) - 60);
  downstreamHeartbeat.heartbeat.signalStrengthDbm = -104;
  downstreamHeartbeat.heartbeat.hasLastLevel = true;
  downstreamHeartbeat.heartbeat.lastLevelMm = 1090;
  site.transmit(downstreamWadiNode, downstreamHeartbeat);

  Packet sirenNodeHeartbeat = Packet();
  sirenNodeHeartbeat.kind = PacketKind::Heartbeat;
  sirenNodeHeartbeat.heartbeat.batteryMillivolts =
      static_cast<uint16_t>(lowBatteryMillivolts(gateway, sirenNode.id) - 320);
  sirenNodeHeartbeat.heartbeat.signalStrengthDbm = -88;
  sirenNodeHeartbeat.heartbeat.tamper = true;
  sirenNodeHeartbeat.heartbeat.alertRefreshLost = true;
  site.transmit(sirenNode, sirenNodeHeartbeat);
  site.runMinutes(1);

  // The monthly siren test.
  Packet test = Packet();
  test.kind = PacketKind::Test;
  site.transmit(sirenNode, test);
  site.runMinutes(1);

  std::vector<std::string> output = site.uplink.published();

  // The all clear is a packet the gateway sends, and it does not send one yet. Until it
  // does, its messages are taken from the translation the gateway core forwards with,
  // for the packets the gateway's own codec signs: one all clear for each wadi node
  // in alert. Once the gateway sends the all clear, produce these lines through its
  // ports like the others.
  FakeStorage gatewayCounters;
  PacketCodec gatewayCodec(site.config.auth, gateway.id, gatewayCounters);
  for (const NodeId cleared : {wadiNode.id, downstreamWadiNode.id}) {
    Packet allClear = Packet();
    allClear.kind = PacketKind::AllClear;
    allClear.allClear.wadiNode = cleared;
    uint8_t frame[wadiwatch::kMaxPacketSize];
    TEST_ASSERT_TRUE(gatewayCodec.encode(allClear, frame, sizeof(frame)) > 0);
    char message[wadiwatch::kMaxUplinkMessageLength + 1];
    const size_t length = wadiwatch::writeUplinkMessage(
        allClear, site.wallClock.unixSeconds(), gateway, message, sizeof(message));
    TEST_ASSERT_TRUE(length > 0);
    output.push_back(std::string(message, length));
  }

  return output;
}

// Where the contract examples are. The tests are started from firmware/ (pio test) or
// from the repository root (pio test -d firmware); anywhere else, name the directory
// in WADIWATCH_CONTRACT_DIR.
std::string contractDirectory() {
  if (const char* named = std::getenv(kDirectoryVariable)) return named;
  for (const char* candidate : {"../docs/gateway-contract", "docs/gateway-contract"}) {
    if (std::ifstream(std::string(candidate) + "/" + kLandmarkFile).good()) return candidate;
  }
  TEST_FAIL_MESSAGE("docs/gateway-contract not found: set WADIWATCH_CONTRACT_DIR");
  return std::string();
}

std::vector<std::string> linesOf(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  TEST_ASSERT_TRUE_MESSAGE(file.good(), ("cannot read " + path).c_str());
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(file, line)) {
    // A checkout on Windows may have turned the line ends into CR LF.
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) lines.push_back(line);
  }
  return lines;
}

void write(const std::string& path, const std::vector<std::string>& lines) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  for (const std::string& line : lines) file << line << '\n';
  TEST_ASSERT_TRUE_MESSAGE(file.good(), ("cannot write " + path).c_str());
}

}  // namespace

static void test_the_gateway_output_file_is_what_the_gateway_core_produces() {
  const std::vector<std::string> produced = gatewayOutput();
  const std::string path = contractDirectory() + "/" + kOutputFile;
  if (std::getenv(kUpdateVariable) != nullptr) write(path, produced);

  const std::vector<std::string> committed = linesOf(path);

  const char* const advice =
      "docs/gateway-contract/gateway-output.jsonl is not what the gateway produces. If the "
      "change is intended, rerun with WADIWATCH_UPDATE_CONTRACT=1 and run the backend tests.";
  for (size_t i = 0; i < produced.size() && i < committed.size(); ++i) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE(committed[i].c_str(), produced[i].c_str(), advice);
  }
  TEST_ASSERT_EQUAL_UINT_MESSAGE(committed.size(), produced.size(), advice);
}

static void test_the_gateway_output_covers_every_message_kind() {
  const std::vector<std::string> produced = gatewayOutput();

  for (const char* kind :
       {"reading", "alert", "acknowledgement", "heartbeat", "all_clear", "test"}) {
    const std::string field = std::string("\"kind\":\"") + kind + "\"";
    bool found = false;
    for (const std::string& message : produced) {
      if (message.find(field) != std::string::npos) found = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(found, kind);
  }
}

// The backend refuses a message from a node its site file does not list, so the file
// must speak for every node of the firmware site configuration that transmits.
static void test_the_gateway_output_covers_every_node_the_gateway_hears() {
  const std::vector<std::string> produced = gatewayOutput();
  const wadiwatch::SiteConfig& site = wadiwatch::prototypeSite();

  for (const NodeId node : {site.wadiNode.id, site.downstreamWadiNode.id, site.sirenNode.id}) {
    const std::string field = "\"node\":" + std::to_string(node) + ",";
    bool found = false;
    for (const std::string& message : produced) {
      if (message.find(field) != std::string::npos) found = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(found, std::to_string(node).c_str());
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_gateway_output_file_is_what_the_gateway_core_produces);
  RUN_TEST(test_the_gateway_output_covers_every_message_kind);
  RUN_TEST(test_the_gateway_output_covers_every_node_the_gateway_hears);
  return UNITY_END();
}
