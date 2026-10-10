// Fake ports for running the device cores on a PC.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "wadiwatch/level_measurement.h"
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
// Every call is one ping. Pings see the water unless a scenario scripts something else
// for them: debris or spray in the beam, a reading no wadi could give, no echo at all.
class FakeLevelSensor : public LevelSensor {
 public:
  // `referenceTemperatureC` is the air temperature the sensor works out its distances for.
  FakeLevelSensor(float mountingHeightCm, float referenceTemperatureC)
      : mountingHeightCm_(mountingHeightCm),
        referenceTemperatureC_(referenceTemperatureC),
        airTemperatureC_(referenceTemperatureC) {}

  void setWaterLevelCm(float levelCm) { waterLevelCm_ = levelCm; }

  // The air over the wadi. The sensor times its echo and knows nothing of the air:
  // in warmer air sound is faster, the echo is back sooner, and the sensor reports
  // the water nearer than it is. Until this is called the air is at the sensor's
  // reference temperature and the reported distances are exact.
  void setAirTemperatureC(float temperatureC) { airTemperatureC_ = temperatureC; }

  // The next ping not yet scripted is returned by something at this height above
  // the bed instead of by the water. Scripted pings are used up one per ping, in order.
  void scriptPingLevelCm(float levelCm) {
    script_.push_back({ScriptedPing::Echo, reported(mountingHeightCm_ - levelCm)});
  }

  // The next ping not yet scripted makes the sensor report exactly this distance,
  // whatever is in front of it.
  void scriptPingDistanceCm(float distanceCm) {
    script_.push_back({ScriptedPing::Echo, distanceCm});
  }

  // The next ping not yet scripted gets no echo: the sensor has no reading.
  void scriptPingWithoutEcho() { script_.push_back({ScriptedPing::NoEcho, 0.0f}); }

  // From now on no ping gets an echo (a dead or unplugged sensor), or pings work again.
  void setEchoing(bool echoing) { echoing_ = echoing; }

  size_t scriptedPingsLeft() const { return script_.size(); }

  bool readDistanceCm(float& distanceCm) override {
    ++readCount_;
    if (!script_.empty()) {
      const ScriptedPing ping = script_.front();
      script_.pop_front();
      if (ping.kind == ScriptedPing::NoEcho) return false;
      distanceCm = ping.distanceCm;
      return true;
    }
    if (!echoing_) return false;
    distanceCm = reported(mountingHeightCm_ - waterLevelCm_);
    return true;
  }

  // How many times the device has pinged the sensor.
  size_t readCount() const { return readCount_; }

 private:
  struct ScriptedPing {
    enum Kind { Echo, NoEcho };
    Kind kind;
    float distanceCm;
  };

  // What the sensor makes of an echo from this far away, in the air as it is.
  float reported(float distanceCm) const {
    if (airTemperatureC_ == referenceTemperatureC_) return distanceCm;
    return distanceCm * speedOfSoundMps(referenceTemperatureC_) / speedOfSoundMps(airTemperatureC_);
  }

  float mountingHeightCm_;
  float referenceTemperatureC_;
  float airTemperatureC_;
  float waterLevelCm_ = 0;
  bool echoing_ = true;
  std::deque<ScriptedPing> script_;
  size_t readCount_ = 0;
};

// The thermometer beside the level sensor. It has no reading until a scenario gives it one.
class FakeTemperatureSensor : public TemperatureSensor {
 public:
  void setTemperatureC(float temperatureC) {
    available_ = true;
    temperatureC_ = temperatureC;
  }

  // The thermometer stops answering.
  void setUnavailable() { available_ = false; }

  bool readTemperatureC(float& temperatureC) override {
    if (!available_) return false;
    temperatureC = temperatureC_;
    return true;
  }

 private:
  bool available_ = false;
  float temperatureC_ = 0;
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

// The button under the siren node's cover, with a technician's finger on it.
class FakeButton : public Button {
 public:
  void press() { ++unreportedPresses_; }

  bool wasPressed() override {
    if (unreportedPresses_ == 0) return false;
    --unreportedPresses_;
    return true;
  }

 private:
  size_t unreportedPresses_ = 0;
};

// Chance with the dice loaded: the scenario says what each draw gives.
class FakeRandom : public RandomSource {
 public:
  // The lowest and the highest number any draw can give, whatever its bound.
  enum : uint32_t { kLowest = 0, kHighest = UINT32_MAX };

  // The coming draws, in order. A draw too high for its bound gives the highest
  // number the bound allows. Once these are used up every draw gives the lowest.
  void setDraws(const std::vector<uint32_t>& draws) { draws_.assign(draws.begin(), draws.end()); }

  uint32_t below(uint32_t bound) override {
    if (draws_.empty()) return kLowest;
    const uint32_t draw = draws_.front();
    draws_.pop_front();
    return draw < bound ? draw : bound - 1;
  }

 private:
  std::deque<uint32_t> draws_;
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

  void setListening(bool listening) override { listening_ = listening; }

  size_t receive(uint8_t* buffer, size_t capacity) override {
    if (inbox_.empty()) return 0;
    const Frame frame = inbox_.front();
    inbox_.pop_front();
    if (frame.size() > capacity) return 0;
    std::memcpy(buffer, frame.data(), frame.size());
    return frame.size();
  }

  // Whether the receiver is on, which is what it costs the battery.
  bool isListening() const { return listening_; }

  // Every frame this radio transmitted, including ones lost on the way.
  const std::vector<Frame>& sent() const { return sent_; }

 private:
  friend class Air;
  InMemoryRadio(Air& air, FakeSleepControl* wakes) : air_(air), wakes_(wakes) {}

  Air& air_;
  // The device this radio's interrupt line wakes when a frame arrives, if any.
  FakeSleepControl* wakes_;
  bool listening_ = true;
  std::deque<Frame> inbox_;
  std::vector<Frame> sent_;
};

// The shared radio medium: what one radio sends, every other radio with its receiver on hears.
class Air {
 public:
  // `wakes` is the sleep control of the device the radio belongs to, for a device
  // that sleeps: a frame arriving while its receiver is on wakes it.
  InMemoryRadio& join(FakeSleepControl* wakes = nullptr) {
    radios_.push_back(std::unique_ptr<InMemoryRadio>(new InMemoryRadio(*this, wakes)));
    return *radios_.back();
  }

  // The next `count` transmissions are lost before reaching any receiver.
  void dropNext(size_t count) { framesToDrop_ += count; }

  // Delivers a frame to every listening radio, as if sent by a transmitter outside the site.
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
      if (radio.get() == sender || !radio->listening_) continue;
      radio->inbox_.push_back(frame);
      if (radio->wakes_ != nullptr) radio->wakes_->wake();
    }
  }

  std::vector<std::unique_ptr<InMemoryRadio>> radios_;
  std::vector<Frame> transmitted_;
  size_t framesToDrop_ = 0;
};

inline void InMemoryRadio::send(const uint8_t* data, size_t length) {
  sent_.push_back(Frame(data, data + length));
  air_.transmit(sent_.back(), this);
}

struct Sms {
  std::string phoneNumber;
  std::string text;
};

// The mobile network as the gateway's modem sees it. Records every SMS it is
// asked to send: those it accepted and, during an outage, those it refused.
class FakeGsmModem : public GsmModem {
 public:
  bool sendSms(const char* phoneNumber, const char* text) override {
    const Sms sms = {phoneNumber, text};
    if (outage_ || isRejected(sms.phoneNumber)) {
      refused_.push_back(sms);
      return false;
    }
    sent_.push_back(sms);
    return true;
  }

  // During an outage the modem has no network and refuses every SMS.
  void startOutage() { outage_ = true; }
  void endOutage() { outage_ = false; }

  // From now on the network refuses every SMS to this number, outage or not.
  void rejectNumber(const std::string& phoneNumber) { rejectedNumbers_.push_back(phoneNumber); }

  // The SMS that went out to the network, in order.
  const std::vector<Sms>& sent() const { return sent_; }
  // The SMS the gateway asked for and the modem refused, in order.
  const std::vector<Sms>& refused() const { return refused_; }

 private:
  bool isRejected(const std::string& phoneNumber) const {
    for (const std::string& rejected : rejectedNumbers_) {
      if (rejected == phoneNumber) return true;
    }
    return false;
  }

  bool outage_ = false;
  std::vector<std::string> rejectedNumbers_;
  std::vector<Sms> sent_;
  std::vector<Sms> refused_;
};

// The time of day as the gateway learns it from the network. The simulated clock
// keeps the true time; a scenario decides whether the gateway knows it.
class FakeWallClock : public WallClock {
 public:
  // The time of day when the simulated clock reads zero: 2026-10-10 00:00:00 UTC.
  // Any fixed instant would do.
  static constexpr uint32_t kStartUnixSeconds = 1791590400u;

  explicit FakeWallClock(const Clock& clock) : clock_(clock) {}

  bool nowUnixSeconds(uint32_t& unixSeconds) override {
    if (!known_) return false;
    unixSeconds = this->unixSeconds();
    return true;
  }

  // The true time of day, whether or not the gateway knows it.
  uint32_t unixSeconds() const {
    return kStartUnixSeconds + static_cast<uint32_t>(clock_.nowMs() / 1000);
  }

  // As after power-on with no network yet: the gateway cannot tell the time.
  void forgetTime() { known_ = false; }
  void learnTime() { known_ = true; }

 private:
  const Clock& clock_;
  bool known_ = true;
};

// The backend as the gateway's uplink sees it. Records every message it took, in order.
class FakeUplink : public Uplink {
 public:
  bool publish(const char* message, size_t length) override {
    ++attempts_;
    if (outage_) return false;
    published_.push_back(std::string(message, length));
    return true;
  }

  // During an outage the uplink is offline and takes nothing.
  void startOutage() { outage_ = true; }
  void endOutage() { outage_ = false; }

  // The messages that reached the backend, in order.
  const std::vector<std::string>& published() const { return published_; }
  // How many times the gateway offered a message, taken or not.
  size_t attempts() const { return attempts_; }

 private:
  bool outage_ = false;
  size_t attempts_ = 0;
  std::vector<std::string> published_;
};

// Stands for the gateway's flash: slots that outlive the core they are given to.
class FakeSlotStorage : public SlotStorage {
 public:
  explicit FakeSlotStorage(size_t slotCount) : slotCount_(slotCount) {}

  // Fits a store of another size. Only before the device first runs.
  void setSlotCount(size_t slotCount) { slotCount_ = slotCount; }

  size_t slotCount() override { return slotCount_; }

  bool read(size_t slot, uint8_t* data) override {
    const auto found = slots_.find(slot);
    if (slot >= slotCount_ || found == slots_.end()) return false;
    std::memcpy(data, found->second.data(), kStorageSlotSize);
    return true;
  }

  bool write(size_t slot, const uint8_t* data) override {
    if (slot >= slotCount_) return false;
    slots_[slot] = Frame(data, data + kStorageSlotSize);
    return true;
  }

  // What worn flash or a power cut in the middle of a write can leave behind: one
  // bit is flipped in one slot that has been written. Returns false when no slot
  // has been written yet.
  bool damageOneSlot() {
    if (slots_.empty()) return false;
    slots_.begin()->second[kStorageSlotSize / 4] ^= 0x01;
    return true;
  }

 private:
  size_t slotCount_;
  std::map<size_t, Frame> slots_;
};

}  // namespace testing
}  // namespace wadiwatch
