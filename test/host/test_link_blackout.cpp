// Host test: the link blackout (ticket 27) — a time-bounded suppression of
// frame forwarding, used to provoke the HMI's initialisation burst on demand.
// Driven purely by BusIo + a mock clock, like test_relay.cpp.
//
// Built against atlantic_v5_transport_blackout, the only library in the suite
// compiled with the gate flag set.
#ifndef ATLANTIC_V5_LINK_BLACKOUT
#error "test_link_blackout must be built with ATLANTIC_V5_LINK_BLACKOUT defined"
#endif

#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

#include "bus_io.h"
#include "crc16.h"
#include "frame.h"
#include "relay.h"
#include "relay_policy.h"
#include "test_harness.h"
#include "types.h"

namespace {

using atlantic_v5::BlackoutDirection;
using atlantic_v5::BusIo;
using atlantic_v5::Channel;
using atlantic_v5::Frame;
using atlantic_v5::Relay;
using atlantic_v5::RelayPolicy;

// Same shape as test_relay.cpp's double: reads are timestamped chunks released
// once the mock clock reaches them, writes are recorded for assertions.
class MockBusIo : public BusIo {
 public:
  void queue(uint32_t t_us, std::vector<uint8_t> bytes) { pending_.push_back({t_us, std::move(bytes)}); }

  int read(uint8_t *dst, size_t max, uint32_t /*timeout_us*/) override {
    if (pending_.empty() || pending_.front().t_us > now_)
      return 0;
    auto &front = pending_.front();
    size_t n = std::min(max, front.bytes.size());
    std::memcpy(dst, front.bytes.data(), n);
    pending_.pop_front();
    return static_cast<int>(n);
  }

  void write(const uint8_t *src, size_t len) override { writes_.push_back(std::vector<uint8_t>(src, src + len)); }
  void flush_input() override {}
  uint32_t now_us() override { return now_; }
  void set_now(uint32_t t) { now_ = t; }

  std::vector<std::vector<uint8_t>> writes_;

 private:
  struct Chunk {
    uint32_t t_us;
    std::vector<uint8_t> bytes;
  };
  std::deque<Chunk> pending_;
  uint32_t now_ = 0;
};

void append_crc(std::vector<uint8_t> &f) {
  uint16_t crc = atlantic_v5::crc16_modbus(f.data(), f.size() - 2);
  f[f.size() - 2] = static_cast<uint8_t>(crc & 0xFF);
  f[f.size() - 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
}

// The two halves of one transaction on header 0x0164FEBD03. On the HMI channel a
// byte-1 of 0x64 makes the frame payload-less and complete at 7 bytes; MAIN's
// answer to the same header carries the length byte and a payload.
std::vector<uint8_t> make_hmi_request() {
  std::vector<uint8_t> f = {0x01, 0x64, 0xFE, 0xBD, 0x03, 0x00, 0x00};
  append_crc(f);
  return f;
}

std::vector<uint8_t> make_main_answer() {
  std::vector<uint8_t> f = {0x01, 0x64, 0xFE, 0xBD, 0x03, 0x02, 0x11, 0x22, 0x00, 0x00};
  append_crc(f);
  return f;
}

// Drives one poll with a frame arriving on `channel` at t_us.
struct Rig {
  MockBusIo hmi_io, main_io;
  RelayPolicy policy;
  Relay relay;

  explicit Rig(Relay::Config cfg) : relay(hmi_io, main_io, policy, cfg) {}

  void feed(Channel channel, const std::vector<uint8_t> &bytes, uint32_t t_us) {
    (channel == Channel::HMI ? hmi_io : main_io).queue(t_us, bytes);
    advance(t_us);
  }

  // Advances both the mock BusIo clock and the relay's own poll clock.
  void advance(uint32_t t_us) {
    hmi_io.set_now(t_us);
    main_io.set_now(t_us);
    relay.poll(t_us);
  }
};

Relay::Config config_with(BlackoutDirection dir, uint32_t blackout_us) {
  Relay::Config cfg;
  cfg.blackout_direction = dir;
  cfg.blackout_us = blackout_us;
  return cfg;
}

const std::vector<uint8_t> HMI_FRAME = make_hmi_request();
const std::vector<uint8_t> MAIN_FRAME = make_main_answer();

}  // namespace

int main() {
  // --- Defaults: no blackout requested, so forwarding is exactly as before. ---
  {
    Rig rig{Relay::Config{}};
    CHECK(!rig.relay.blackout_engaged());
    rig.feed(Channel::HMI, HMI_FRAME, 1000);
    rig.feed(Channel::MAIN, MAIN_FRAME, 2000);
    CHECK(rig.main_io.writes_.size() == 1);
    CHECK(rig.hmi_io.writes_.size() == 1);
    CHECK(rig.relay.stats().frames_relayed == 2);
  }

  // --- Each direction drops only the frames travelling that way; the opposite
  // direction keeps flowing untouched. ---
  {
    struct Case {
      BlackoutDirection dir;
      bool hmi_to_main_forwarded;
      bool main_to_hmi_forwarded;
    };
    const Case cases[] = {
        {BlackoutDirection::MAIN_TO_HMI, true, false},
        {BlackoutDirection::HMI_TO_MAIN, false, true},
        {BlackoutDirection::BOTH, false, false},
    };

    for (const auto &c : cases) {
      Rig rig{config_with(c.dir, 15'000'000)};
      rig.relay.request_blackout();
      rig.advance(1000);  // the poll that latches it
      CHECK(rig.relay.blackout_engaged());

      rig.feed(Channel::HMI, HMI_FRAME, 2000);
      rig.feed(Channel::MAIN, MAIN_FRAME, 3000);

      CHECK(rig.main_io.writes_.size() == (c.hmi_to_main_forwarded ? 1u : 0u));
      CHECK(rig.hmi_io.writes_.size() == (c.main_to_hmi_forwarded ? 1u : 0u));
      // A dropped frame was never relayed, so it must not be counted as one.
      uint32_t expected = (c.hmi_to_main_forwarded ? 1u : 0u) + (c.main_to_hmi_forwarded ? 1u : 0u);
      CHECK(rig.relay.stats().frames_relayed == expected);
      // Framing is untouched by the cut: both frames were still assembled.
      CHECK(rig.relay.hmi_stats().valid_frames == 1);
      CHECK(rig.relay.main_stats().valid_frames == 1);
    }
  }

  // --- Auto-release runs on the relay's own clock. Nothing but poll() can
  // clear the blackout, and poll() clears it without any help from the caller
  // that requested it. ---
  {
    Rig rig{config_with(BlackoutDirection::BOTH, 15'000'000)};
    rig.relay.request_blackout();
    rig.advance(1'000);
    CHECK(rig.relay.blackout_engaged());

    // Mock time running far past the window with no poll leaves it engaged: the
    // countdown belongs to the relay task, not to wall-clock time.
    rig.hmi_io.set_now(1'000'000'000);
    rig.main_io.set_now(1'000'000'000);
    CHECK(rig.relay.blackout_engaged());

    // One microsecond short of the window: still cut.
    rig.advance(1'000 + 15'000'000 - 1);
    CHECK(rig.relay.blackout_engaged());
    rig.feed(Channel::MAIN, MAIN_FRAME, 1'000 + 15'000'000 - 1);
    CHECK(rig.hmi_io.writes_.empty());

    // At the window, the relay releases itself and forwarding resumes.
    rig.advance(1'000 + 15'000'000);
    CHECK(!rig.relay.blackout_engaged());
    rig.feed(Channel::MAIN, MAIN_FRAME, 1'000 + 15'000'001);
    CHECK(rig.hmi_io.writes_.size() == 1);
  }

  // --- Auto-release survives the 32-bit microsecond wrap (~71.6 min), the same
  // hazard the echo window carries. ---
  {
    Rig rig{config_with(BlackoutDirection::BOTH, 15'000'000)};
    const uint32_t start = 0xFFFF'FF00u;
    rig.relay.request_blackout();
    rig.advance(start);
    CHECK(rig.relay.blackout_engaged());

    // Past the wrap but inside the window.
    rig.advance(start + 1'000'000);
    CHECK(rig.relay.blackout_engaged());

    rig.advance(start + 15'000'000);
    CHECK(!rig.relay.blackout_engaged());
  }

  // --- Manual release: turning the switch off cuts the window short. ---
  {
    Rig rig{config_with(BlackoutDirection::BOTH, 15'000'000)};
    rig.relay.request_blackout();
    rig.advance(1'000);
    CHECK(rig.relay.blackout_engaged());

    rig.relay.release_blackout();
    CHECK(!rig.relay.blackout_engaged());
    rig.feed(Channel::MAIN, MAIN_FRAME, 2'000);
    CHECK(rig.hmi_io.writes_.size() == 1);
  }

  // --- A second request after an auto-release starts a fresh window rather
  // than staying latched on the first one's start time. ---
  {
    Rig rig{config_with(BlackoutDirection::BOTH, 1'000'000)};
    rig.relay.request_blackout();
    rig.advance(1'000);
    rig.advance(1'000 + 1'000'000);
    CHECK(!rig.relay.blackout_engaged());

    rig.relay.request_blackout();
    rig.advance(1'000 + 1'000'001);
    CHECK(rig.relay.blackout_engaged());
    rig.advance(1'000 + 1'000'001 + 999'999);
    CHECK(rig.relay.blackout_engaged());
    rig.advance(1'000 + 1'000'001 + 1'000'000);
    CHECK(!rig.relay.blackout_engaged());
  }

  // --- Observability survives the cut: a dropped frame still reaches the frame
  // sink, because the frames that were not forwarded are exactly the evidence a
  // blackout exists to collect. ---
  {
    static std::vector<Channel> seen;
    seen.clear();
    Rig rig{config_with(BlackoutDirection::BOTH, 15'000'000)};
    rig.relay.set_frame_sink(
        [](void *, Channel channel, const Frame &, const uint8_t *, uint32_t) { seen.push_back(channel); }, nullptr);

    rig.relay.request_blackout();
    rig.advance(1'000);
    rig.feed(Channel::HMI, HMI_FRAME, 2'000);
    rig.feed(Channel::MAIN, MAIN_FRAME, 3'000);

    CHECK(rig.hmi_io.writes_.empty() && rig.main_io.writes_.empty());
    CHECK(seen.size() == 2);
    CHECK(seen[0] == Channel::HMI && seen[1] == Channel::MAIN);
  }

  TEST_MAIN_RETURN();
}
