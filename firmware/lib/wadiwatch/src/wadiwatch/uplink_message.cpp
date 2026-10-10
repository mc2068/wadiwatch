#include "wadiwatch/uplink_message.h"

namespace wadiwatch {

namespace {

constexpr uint32_t kContractVersion = 1;

// Writes one flat JSON object into a fixed buffer, field by field. Names and
// text values are the fixed words of the contract, so nothing needs escaping.
class JsonObject {
 public:
  JsonObject(char* buffer, size_t capacity) : buffer_(buffer), capacity_(capacity) {}

  void number(const char* name, uint32_t value) {
    field(name);
    digits(value);
  }

  void signedNumber(const char* name, int32_t value) {
    field(name);
    sign(value);
    digits(magnitude(value));
  }

  // A value held in tenths, written with one decimal: 845 is 84.5.
  void tenths(const char* name, int32_t value) {
    field(name);
    sign(value);
    digits(magnitude(value) / 10);
    put('.');
    put(static_cast<char>('0' + magnitude(value) % 10));
  }

  void text(const char* name, const char* value) {
    field(name);
    put('"');
    put(value);
    put('"');
  }

  void flag(const char* name, bool value) {
    field(name);
    put(value ? "true" : "false");
  }

  // Closes the object. Returns its length, or 0 if it did not fit.
  size_t finish() {
    put('}');
    if (overflowed_ || length_ >= capacity_) return 0;
    buffer_[length_] = '\0';
    return length_;
  }

 private:
  static uint32_t magnitude(int32_t value) {
    return value < 0 ? 0u - static_cast<uint32_t>(value) : static_cast<uint32_t>(value);
  }

  void field(const char* name) {
    put(length_ == 0 ? '{' : ',');
    put('"');
    put(name);
    put("\":");
  }

  void sign(int32_t value) {
    if (value < 0) put('-');
  }

  void digits(uint32_t value) {
    char reversed[10];
    size_t count = 0;
    do {
      reversed[count++] = static_cast<char>('0' + value % 10);
      value /= 10;
    } while (value > 0);
    while (count > 0) put(reversed[--count]);
  }

  void put(const char* text) {
    while (*text != '\0') put(*text++);
  }

  void put(char c) {
    if (length_ < capacity_) {
      buffer_[length_++] = c;
    } else {
      overflowed_ = true;
    }
  }

  char* buffer_;
  size_t capacity_;
  size_t length_ = 0;
  bool overflowed_ = false;
};

const char* nameOf(PacketKind kind) {
  switch (kind) {
    case PacketKind::Reading:
      return "reading";
    case PacketKind::Alert:
      return "alert";
    case PacketKind::Acknowledgement:
      return "acknowledgement";
    case PacketKind::Heartbeat:
      return "heartbeat";
    case PacketKind::AllClear:
      return "all_clear";
    case PacketKind::Test:
      return "test";
  }
  return nullptr;
}

const char* nameOf(AlertLevel alertLevel) {
  switch (alertLevel) {
    case AlertLevel::None:
      return "none";
    case AlertLevel::Watch:
      return "watch";
    case AlertLevel::Warning:
      return "warning";
    case AlertLevel::Evacuate:
      return "evacuate";
  }
  return nullptr;
}

// Heartbeat packets carry the voltage; the verdict is the site configuration's.
bool isLowBattery(const GatewayConfig& config, NodeId node, uint16_t batteryMillivolts) {
  const size_t count = config.batteryCount < kMaxSiteNodes ? config.batteryCount : kMaxSiteNodes;
  for (size_t i = 0; i < count; ++i) {
    if (config.batteries[i].node == node) {
      return batteryMillivolts < config.batteries[i].lowBatteryMillivolts;
    }
  }
  return false;
}

}  // namespace

// Packets count in whole millimetres, tenths of a millimetre of rain and millimetres
// per minute; the contract in centimetres, millimetres of rain and centimetres per
// minute. Each is the packet's number with one decimal, so nothing is rounded.
size_t writeUplinkMessage(const Packet& packet,
                          uint32_t unixSeconds,
                          const GatewayConfig& config,
                          char* buffer,
                          size_t capacity) {
  const char* kind = nameOf(packet.kind);
  if (kind == nullptr) return 0;

  JsonObject message(buffer, capacity);
  message.number("v", kContractVersion);
  message.text("kind", kind);
  message.number("node", packet.sender);
  message.number("counter", packet.counter);
  message.number("ts", unixSeconds);

  switch (packet.kind) {
    case PacketKind::Reading: {
      const ReadingPayload& reading = packet.reading;
      const char* alertLevel = nameOf(reading.alertLevel);
      if (alertLevel == nullptr) return 0;
      message.tenths("level_cm", reading.levelMm);
      message.tenths("rise_cm_min", reading.rateOfRiseMmPerMin);
      message.tenths("rain_mm", reading.rainfallTenthsMm);
      message.flag("storm_mode", reading.stormMode);
      message.text("alert_level", alertLevel);
      break;
    }
    case PacketKind::Alert: {
      const AlertPayload& alert = packet.alert;
      const char* alertLevel = nameOf(alert.alertLevel);
      if (alertLevel == nullptr) return 0;
      message.text("alert_level", alertLevel);
      message.tenths("level_cm", alert.levelMm);
      message.tenths("rise_cm_min", alert.rateOfRiseMmPerMin);
      message.flag("refresh", alert.refresh);
      break;
    }
    case PacketKind::Acknowledgement: {
      const AcknowledgementPayload& acknowledgement = packet.acknowledgement;
      const char* alertLevel = nameOf(acknowledgement.alertLevel);
      if (alertLevel == nullptr) return 0;
      message.number("wadi_node", acknowledgement.alertSender);
      message.text("alert_level", alertLevel);
      message.number("alert_counter", acknowledgement.alertCounter);
      break;
    }
    case PacketKind::Heartbeat: {
      const HeartbeatPayload& heartbeat = packet.heartbeat;
      message.number("battery_mv", heartbeat.batteryMillivolts);
      message.signedNumber("rssi_dbm", heartbeat.signalStrengthDbm);
      message.flag("low_battery",
                   isLowBattery(config, packet.sender, heartbeat.batteryMillivolts));
      message.flag("tamper", heartbeat.tamper);
      // A node that has measured no level has none to report.
      if (heartbeat.hasLastLevel) message.tenths("level_cm", heartbeat.lastLevelMm);
      message.number("discards", heartbeat.discardedReadings);
      message.flag("refresh_lost", heartbeat.alertRefreshLost);
      break;
    }
    case PacketKind::AllClear:
      message.number("wadi_node", packet.allClear.wadiNode);
      message.number("command", packet.allClear.command);
      break;
    case PacketKind::Test:
      break;
  }
  return message.finish();
}

}  // namespace wadiwatch
