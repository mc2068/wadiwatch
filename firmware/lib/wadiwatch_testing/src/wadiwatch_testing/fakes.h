// Fake ports for running the device cores on a PC.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <vector>

#include "wadiwatch/ports.h"

namespace wadiwatch {
namespace testing {

using Frame = std::vector<uint8_t>;

class FakeClock : public Clock {
 public:
  uint64_t nowMs() const override { return nowMs_; }
  void advanceMs(uint64_t ms) { nowMs_ += ms; }

 private:
  uint64_t nowMs_ = 0;
};

// Scenarios speak in water level; the fake reports the distance a real sensor would see.
class FakeLevelSensor : public LevelSensor {
 public:
  explicit FakeLevelSensor(float mountingHeightCm) : mountingHeightCm_(mountingHeightCm) {}

  void setWaterLevelCm(float levelCm) { waterLevelCm_ = levelCm; }

  bool readDistanceCm(float& distanceCm) override {
    distanceCm = mountingHeightCm_ - waterLevelCm_;
    return true;
  }

 private:
  float mountingHeightCm_;
  float waterLevelCm_ = 0;
};

class FakeSirenOutput : public SirenOutput {
 public:
  void setSiren(bool on) override { on_ = on; }
  bool isOn() const { return on_; }

 private:
  bool on_ = false;
};

// Stands for a device's flash: it outlives the core it is given to, as flash outlives a restart.
class FakeStorage : public PersistentStorage {
 public:
  bool load(uint32_t key, uint32_t& value) override {
    const auto found = values_.find(key);
    if (found == values_.end()) return false;
    value = found->second;
    return true;
  }

  void store(uint32_t key, uint32_t value) override { values_[key] = value; }

 private:
  std::map<uint32_t, uint32_t> values_;
};

class Air;

// One device's radio. Created by Air::join().
class InMemoryRadio : public Radio {
 public:
  void send(const uint8_t* data, size_t length) override;

  size_t receive(uint8_t* buffer, size_t capacity) override {
    if (inbox_.empty()) return 0;
    const Frame frame = inbox_.front();
    inbox_.pop_front();
    if (frame.size() > capacity) return 0;
    std::memcpy(buffer, frame.data(), frame.size());
    return frame.size();
  }

 private:
  friend class Air;
  explicit InMemoryRadio(Air& air) : air_(air) {}

  Air& air_;
  std::deque<Frame> inbox_;
};

// The shared radio medium: what one radio sends, every other radio receives.
class Air {
 public:
  InMemoryRadio& join() {
    radios_.push_back(std::unique_ptr<InMemoryRadio>(new InMemoryRadio(*this)));
    return *radios_.back();
  }

  // The next `count` transmissions are lost before reaching any receiver.
  void dropNext(size_t count) { framesToDrop_ += count; }

  // Delivers a frame to every radio, as if sent by a transmitter outside the site.
  void inject(const Frame& frame) { deliver(frame, nullptr); }

  // Every frame transmitted by a joined radio, including dropped ones.
  const std::vector<Frame>& transmitted() const { return transmitted_; }

 private:
  friend class InMemoryRadio;

  void transmit(const Frame& frame, const InMemoryRadio* sender) {
    transmitted_.push_back(frame);
    if (framesToDrop_ > 0) {
      --framesToDrop_;
      return;
    }
    deliver(frame, sender);
  }

  void deliver(const Frame& frame, const InMemoryRadio* sender) {
    for (auto& radio : radios_) {
      if (radio.get() != sender) radio->inbox_.push_back(frame);
    }
  }

  std::vector<std::unique_ptr<InMemoryRadio>> radios_;
  std::vector<Frame> transmitted_;
  size_t framesToDrop_ = 0;
};

inline void InMemoryRadio::send(const uint8_t* data, size_t length) {
  air_.transmit(Frame(data, data + length), this);
}

}  // namespace testing
}  // namespace wadiwatch
