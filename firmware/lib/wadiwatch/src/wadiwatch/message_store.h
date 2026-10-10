// The gateway's store for messages waiting to be forwarded to the backend. Every packet
// the gateway accepts is kept here, in the order received, until the uplink has taken
// it, so an uplink outage or a restart loses nothing.
//
// The store is bounded. When it is full and another packet arrives, one message gives
// way: the least important one, and among equals the oldest. From least to most
// important:
//
//   1. readings
//   2. heartbeats
//   3. acknowledgements and tests
//   4. alerts that repeat the alert level of the alert stored before them from the same
//      wadi node (the refresh a node at Warning or above sends every minute)
//   5. alerts that change the alert level, and all clears
//
// The arriving packet takes part: if it is less important than everything stored, it is
// the one that is not kept. So a change of alert level is given up only by a store that
// holds nothing but changes of alert level and all clears, and then the oldest goes, so
// that the backend still learns how things stand now.
#pragma once

#include <cstddef>
#include <cstdint>

#include "wadiwatch/packet.h"
#include "wadiwatch/ports.h"

namespace wadiwatch {

// The most messages the store keeps, however many slots the storage has.
constexpr size_t kMaxStoredMessages = 1024;

struct StoredMessage {
  Packet packet;
  // When the gateway received the packet: Unix seconds, or, if the gateway did not
  // know the time of day then, seconds since it started.
  uint32_t receivedAt;
  bool datedSinceStart;
  // Filled in by the store: the message was stored before the gateway last
  // restarted. A time counted since a start that is gone can no longer be placed.
  bool fromEarlierRun;
};

class MessageStore {
 public:
  explicit MessageStore(SlotStorage& storage);

  // Keeps a message behind every message already stored.
  void add(const StoredMessage& message);
  // The message stored longest. Returns false when the store is empty.
  bool oldest(StoredMessage& message);
  // Lets go of the message oldest() returns: the backend has it.
  void removeOldest();

 private:
  struct Entry {
    // Position in the order of arrival. Zero: the slot is free.
    uint32_t sequence;
    // For an alert, its sender; for an all clear, the wadi node it names.
    NodeId wadiNode;
    uint8_t importance;
    AlertLevel alertLevel;
  };

  void loadOnce();
  bool repeatsStoredAlert(NodeId wadiNode, AlertLevel alertLevel) const;
  bool findOldest(size_t& slot) const;
  bool findLeastImportant(size_t& slot) const;
  bool findFree(size_t& slot);
  static bool parse(const uint8_t* data, StoredMessage& message, Entry& entry);

  SlotStorage& storage_;
  bool loaded_ = false;
  size_t slotCount_ = 0;
  // One per slot of the storage: what the slot holds.
  Entry entries_[kMaxStoredMessages];
  uint32_t nextSequence_ = 1;
  // Messages with a lower sequence were found in the storage at start.
  uint32_t firstSequenceOfThisRun_ = 1;
  // Where the search for a free slot resumes, so that writes are spread over the storage.
  size_t nextFreeSearch_ = 0;
};

}  // namespace wadiwatch
