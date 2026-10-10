// A wadi node's link to its upstream node: when to listen for it and what was last heard from it.
#pragma once

#include <cstdint>

#include "wadiwatch/operating_mode.h"
#include "wadiwatch/packet.h"

namespace wadiwatch {

struct UpstreamConfig {
  // The wadi node upstream of this one on the same wadi, or kNoNode when there is none.
  NodeId node;
  // In storm mode the receiver is on for this long after each measurement.
  uint32_t receiveWindowMs;
};

// Keeps the upstream bookkeeping of one wadi node. Pure: the caller passes in the time,
// switches the receiver and hands over the packets its codec accepted.
class UpstreamLink {
 public:
  explicit UpstreamLink(const UpstreamConfig& config);

  // A measurement was taken: a receive window opens.
  void onMeasurement(uint64_t nowMs);

  // Whether the receiver should be on now, for a node in `mode` whose own alert level is
  // `alertLevel`. A node with no upstream node never listens; in dry mode nobody does.
  //  - In storm mode the receiver is on during the receive window after each measurement.
  //  - At Warning it stays on between measurements: the upstream node's next re-send is
  //    all that is missing for Evacuate, and it must not fall outside a short window.
  //  - At Evacuate it is off: nothing the upstream node says can raise the alert level further.
  bool wantsReceiver(uint64_t nowMs, OperatingMode mode, AlertLevel alertLevel) const;

  // When the current receive window ends. The node must be awake then to switch the receiver off.
  uint64_t windowEndsAtMs() const { return windowEndsAtMs_; }

  // A packet the node's codec accepted. Only an alert sent by the upstream node counts.
  void onPacket(const Packet& packet);

  // The last alert level heard from the upstream node; None when there is no upstream
  // node, nothing has been heard from it, or its event has ended since.
  AlertLevel alertLevel() const { return alertLevel_; }

  // The upstream node this link listens for, or kNoNode.
  NodeId node() const { return config_.node; }

  // The event at the upstream node is over: what was heard from it no longer holds.
  void forget() { alertLevel_ = AlertLevel::None; }

 private:
  UpstreamConfig config_;
  uint64_t windowEndsAtMs_ = 0;
  AlertLevel alertLevel_ = AlertLevel::None;
};

}  // namespace wadiwatch
