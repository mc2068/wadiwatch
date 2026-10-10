// Fake ports for running the device cores on a PC.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
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

// Records sleep requests. TestSite leaves a sleeping device alone until its
// time is up or something wakes it.
class FakeSleepControl : public SleepControl {
 public:
  explicit FakeSleepControl(const Clock& clock) : clock_(clock) {}

  void requestSleepMs(uint64_t durationMs) override {
    wakeAtMs_ = clock_.nowMs() + durationMs;
    lastRequestMs_ = durationMs;
    ++requestCount_;
  }

  // What a wake-up interrupt does on the real device.
  void wake() { wakeAtMs_ = 0; }

  bool isAsleep() const { return clock_.nowMs() < wakeAtMs_; }
  // When the sleep last asked for ends.
  uint64_t wakeAtMs() const { return wakeAtMs_; }
  size_t requestCount() const { return requestCount_; }
  uint64_t lastRequestMs() const { return lastRequestMs_; }

 private:
  const Clock& clock_;
  uint64_t wakeAtMs_ = 0;
  uint64_t lastRequestMs_ = 0;
  size_t requestCount_ = 0;
};

// Scenarios speak in water level; the fake reports the distance a real sensor would see.
class FakeLevelSensor : public LevelSensor {
 public:
  explicit FakeLevelSensor(float mountingHeightCm) : mountingHeightCm_(mountingHeightCm) {}

  void setWaterLevelCm(float levelCm) { waterLevelCm_ = levelCm; }

  bool readDistanceCm(float& distanceCm) override {
    ++readCount_;
    distanceCm = mountingHeightCm_ - waterLevelCm_;
    return true;
  }

  // How many times the device has pinged the sensor.
  size_t readCount() const { return readCount_; }

 private:
  float mountingHeightCm_;
  float waterLevelCm_ = 0;
  size_t readCount_ = 0;
};

// A tipping-bucket gauge wired to the device's wake-up pin: every tip wakes a sleeping device.
class FakeRainGauge : public RainGauge {
 public:
  FakeRainGauge(float mmPerTip, FakeSleepControl& sleep) : mmPerTip_(mmPerTip), sleep_(sleep) {}

  void tip(uint32_t tips = 1) {
    if (tips == 0) return;
    tipCount_ += tips;
    sleep_.wake();
  }

  // Scenarios speak in millimetres of rain; the fake turns them into whole tips.
  void rainMm(float mm) { tip(static_cast<uint32_t>(std::lround(mm / mmPerTip_))); }

  uint32_t tipCount() override { return tipCount_; }

 private:
  float mmPerTip_;
  FakeSleepControl& sleep_;
  uint32_t tipCount_ = 0;
};

class FakeNodeHealth : public NodeHealth {
 public:
  void setBatteryMillivolts(uint16_t millivolts) { batteryMillivolts_ = millivolts; }
  void setSignalStrengthDbm(int8_t dbm) { signalStrengthDbm_ = dbm; }
  void setTamper(bool tamper) { tamper_ = tamper; }

  uint16_t batteryMillivolts() override { return batteryMillivolts_; }
  int8_t signalStrengthDbm() override { return signalStrengthDbm_; }
  bool tamperDetected() override { return tamper_; }

 private:
  uint16_t batteryMillivolts_ = 0;
  int8_t signalStrengthDbm_ = 0;
  bool tamper_ = false;
};

class FakeSirenOutput : public SirenOutput {
 public:
  void setSiren(bool on) override { on_ = on; }
  void setStrobe(bool on) override { strobeOn_ = on; }

  // Whether the siren is sounding at this instant. During the intermittent
  // pattern this comes and goes; TestSite::listenToSiren() names the pattern.
  bool isOn() const { return on_; }
  bool isStrobeOn() const { return strobeOn_; }

 private:
  bool on_ = false;
  bool strobeOn_ = false;
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
