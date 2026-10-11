#include "wadiwatch/message_store.h"

#include <cstring>

#include "wadiwatch/byte_order.h"

namespace wadiwatch {

namespace {

// What one slot of the storage holds, multi-byte fields big-endian:
//
//   0      record format
//   1      flags: received-at is counted from the gateway's start 0x01,
//          alert that told the backend nothing new when it was stored 0x02
//   2-5    sequence: position in the order of arrival, never zero
//   6-9    received at
//   10-25  the packet, as packet.h stores one
//   26-27  check over bytes 0-25
//   28-31  unused, zero
//
// A slot whose format or check is wrong holds no message: that is how a slot never
// written, a slot let go, and a slot half written when the power failed all read.
constexpr uint8_t kRecordFormat = 1;
constexpr size_t kFormatOffset = 0;
constexpr size_t kFlagsOffset = 1;
constexpr size_t kSequenceOffset = 2;
constexpr size_t kReceivedAtOffset = 6;
constexpr size_t kPacketOffset = 10;
constexpr size_t kCheckOffset = kPacketOffset + kPacketContentSize;
constexpr size_t kRecordSize = kCheckOffset + 2;
static_assert(kRecordSize <= kStorageSlotSize, "a stored message must fit one slot");

constexpr uint8_t kFlagDatedSinceStart = 0x01;
constexpr uint8_t kFlagNothingNew = 0x02;

// In the order in which messages give way when the store is full.
constexpr uint8_t kReading = 0;
constexpr uint8_t kHeartbeat = 1;
constexpr uint8_t kAcknowledgementOrTest = 2;
constexpr uint8_t kAlertNothingNew = 3;
constexpr uint8_t kAlertLevelChange = 4;

// Fletcher-16: enough to tell a whole record from a torn or faded one.
uint16_t checkOf(const uint8_t* data, size_t length) {
  uint16_t low = 0;
  uint16_t high = 0;
  for (size_t i = 0; i < length; ++i) {
    low = static_cast<uint16_t>((low + data[i]) % 255);
    high = static_cast<uint16_t>((high + low) % 255);
  }
  return static_cast<uint16_t>((high << 8) | low);
}

uint8_t importanceOf(PacketKind kind, bool nothingNew) {
  switch (kind) {
    case PacketKind::Reading:
      return kReading;
    case PacketKind::Heartbeat:
      return kHeartbeat;
    case PacketKind::Acknowledgement:
    case PacketKind::Test:
      return kAcknowledgementOrTest;
    case PacketKind::Alert:
      return nothingNew ? kAlertNothingNew : kAlertLevelChange;
    case PacketKind::AllClear:
      return kAlertLevelChange;
  }
  return kReading;
}

// Which wadi node a message settles the alert level of, and at what level.
// Returns false for a message that says nothing about an alert level.
bool alertLevelStated(const Packet& packet, NodeId& wadiNode, AlertLevel& alertLevel) {
  if (packet.kind == PacketKind::Alert) {
    wadiNode = packet.sender;
    alertLevel = packet.alert.alertLevel;
    return true;
  }
  if (packet.kind == PacketKind::AllClear) {
    // An all clear leaves the wadi node at no alert.
    wadiNode = packet.allClear.wadiNode;
    alertLevel = AlertLevel::None;
    return true;
  }
  return false;
}

}  // namespace

MessageStore::MessageStore(SlotStorage& storage) : storage_(storage) {}

// The storage is read on first use, not in the constructor: on the ESP32 the core is
// built before the flash is ready.
void MessageStore::loadOnce() {
  if (loaded_) return;
  loaded_ = true;

  slotCount_ = storage_.slotCount();
  if (slotCount_ > kMaxStoredMessages) slotCount_ = kMaxStoredMessages;

  uint32_t highest = 0;
  for (size_t slot = 0; slot < slotCount_; ++slot) {
    Entry& entry = entries_[slot];
    entry = Entry();
    uint8_t data[kStorageSlotSize];
    StoredMessage message;
    if (!storage_.read(slot, data) || !parse(data, message, entry)) {
      entry.sequence = 0;
      continue;
    }
    if (entry.sequence > highest) highest = entry.sequence;
  }
  nextSequence_ = highest + 1;
  firstSequenceOfThisRun_ = nextSequence_;
}

void MessageStore::add(const StoredMessage& message) {
  loadOnce();
  if (slotCount_ == 0) return;

  Entry entry = Entry();
  entry.sequence = nextSequence_;
  bool nothingNew = false;
  if (alertLevelStated(message.packet, entry.wadiNode, entry.alertLevel)) {
    nothingNew = message.packet.kind == PacketKind::Alert &&
                tellsNothingNew(entry.wadiNode, entry.alertLevel);
  }
  entry.importance = importanceOf(message.packet.kind, nothingNew);

  size_t slot = 0;
  if (!findFree(slot)) {
    // Full: the least important message gives way, the oldest of them first. If that
    // is the arriving one, it is not kept.
    if (!findLeastImportant(slot)) return;
    if (entry.importance < entries_[slot].importance) return;
  }

  uint8_t data[kStorageSlotSize];
  std::memset(data, 0, sizeof(data));
  if (!writePacketContent(message.packet, data + kPacketOffset)) return;
  data[kFormatOffset] = kRecordFormat;
  if (message.datedSinceStart) data[kFlagsOffset] |= kFlagDatedSinceStart;
  if (nothingNew) data[kFlagsOffset] |= kFlagNothingNew;
  put32(data + kSequenceOffset, entry.sequence);
  put32(data + kReceivedAtOffset, message.receivedAt);
  put16(data + kCheckOffset, checkOf(data, kCheckOffset));

  ++nextSequence_;
  // A slot that could not be written holds nothing to rely on.
  if (!storage_.write(slot, data)) entry.sequence = 0;
  entries_[slot] = entry;
}

bool MessageStore::oldest(StoredMessage& message) {
  loadOnce();
  size_t slot = 0;
  while (findOldest(slot)) {
    uint8_t data[kStorageSlotSize];
    Entry stored;
    if (storage_.read(slot, data) && parse(data, message, stored) &&
        stored.sequence == entries_[slot].sequence) {
      message.fromEarlierRun = stored.sequence < firstSequenceOfThisRun_;
      return true;
    }
    // The slot no longer holds what was written to it. The message is lost; the
    // ones behind it must not wait for it.
    entries_[slot].sequence = 0;
  }
  return false;
}

void MessageStore::removeOldest() {
  loadOnce();
  size_t slot = 0;
  if (!findOldest(slot)) return;
  uint8_t blank[kStorageSlotSize];
  std::memset(blank, 0, sizeof(blank));
  // If this write fails the message is found again after a restart and forwarded a
  // second time, which the backend ignores.
  storage_.write(slot, blank);
  // The backend now knows what this message said of a wadi node's alert level.
  const Entry& entry = entries_[slot];
  if (entry.importance >= kAlertNothingNew) noteTold(entry.wadiNode, entry.alertLevel);
  entries_[slot].sequence = 0;
}

// Reads a slot. Returns false when it holds no message.
bool MessageStore::parse(const uint8_t* data, StoredMessage& message, Entry& entry) {
  if (data[kFormatOffset] != kRecordFormat) return false;
  if (get16(data + kCheckOffset) != checkOf(data, kCheckOffset)) return false;
  const uint32_t sequence = get32(data + kSequenceOffset);
  if (sequence == 0) return false;
  if (!readPacketContent(data + kPacketOffset, message.packet)) return false;

  const uint8_t flags = data[kFlagsOffset];
  message.receivedAt = get32(data + kReceivedAtOffset);
  message.datedSinceStart = (flags & kFlagDatedSinceStart) != 0;
  message.fromEarlierRun = false;

  entry = Entry();
  entry.sequence = sequence;
  alertLevelStated(message.packet, entry.wadiNode, entry.alertLevel);
  entry.importance = importanceOf(message.packet.kind, (flags & kFlagNothingNew) != 0);
  return true;
}

// Whether the backend, once it has everything stored, will already take this wadi node
// to be at `alertLevel`. The latest word on the node's alert level is the newest alert
// or all clear waiting in the store or, when none is waiting, the last one the uplink
// took. What a full store gave up was never said: a wadi node whose change of alert
// level was given up is news to the backend again in its next refresh.
bool MessageStore::tellsNothingNew(NodeId wadiNode, AlertLevel alertLevel) const {
  const Entry* latest = nullptr;
  for (size_t slot = 0; slot < slotCount_; ++slot) {
    const Entry& entry = entries_[slot];
    if (entry.sequence == 0 || entry.importance < kAlertNothingNew) continue;
    if (entry.wadiNode != wadiNode) continue;
    if (latest == nullptr || entry.sequence > latest->sequence) latest = &entry;
  }
  if (latest != nullptr) return latest->alertLevel == alertLevel;
  for (size_t i = 0; i < toldCount_; ++i) {
    if (told_[i].wadiNode == wadiNode) return told_[i].alertLevel == alertLevel;
  }
  return false;
}

void MessageStore::noteTold(NodeId wadiNode, AlertLevel alertLevel) {
  for (size_t i = 0; i < toldCount_; ++i) {
    if (told_[i].wadiNode != wadiNode) continue;
    told_[i].alertLevel = alertLevel;
    return;
  }
  // No room for one more wadi node: its next alert counts as a change, which costs
  // a refresh its low place and nothing else.
  if (toldCount_ >= kMaxSiteNodes) return;
  told_[toldCount_].wadiNode = wadiNode;
  told_[toldCount_].alertLevel = alertLevel;
  ++toldCount_;
}

bool MessageStore::findOldest(size_t& oldest) const {
  bool found = false;
  for (size_t slot = 0; slot < slotCount_; ++slot) {
    const Entry& entry = entries_[slot];
    if (entry.sequence == 0) continue;
    if (!found || entry.sequence < entries_[oldest].sequence) {
      oldest = slot;
      found = true;
    }
  }
  return found;
}

bool MessageStore::findLeastImportant(size_t& least) const {
  bool found = false;
  for (size_t slot = 0; slot < slotCount_; ++slot) {
    const Entry& entry = entries_[slot];
    if (entry.sequence == 0) continue;
    if (!found || entry.importance < entries_[least].importance ||
        (entry.importance == entries_[least].importance &&
         entry.sequence < entries_[least].sequence)) {
      least = slot;
      found = true;
    }
  }
  return found;
}

bool MessageStore::findFree(size_t& free) {
  for (size_t step = 0; step < slotCount_; ++step) {
    const size_t slot = (nextFreeSearch_ + step) % slotCount_;
    if (entries_[slot].sequence != 0) continue;
    free = slot;
    nextFreeSearch_ = (slot + 1) % slotCount_;
    return true;
  }
  return false;
}

}  // namespace wadiwatch
