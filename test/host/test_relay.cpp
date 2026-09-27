// M5 host test (ticket 05): Relay driven purely by BusIo + a mock clock — forwarding
// decisions, the rewrite hook (plan 2.8), echo accounting, and the silence backstop.
// No hardware, no Decoder dependency.
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "bus_io.h"
#include "crc16.h"
#include "frame.h"
#include "relay.h"
#include "relay_policy.h"
#include "test_harness.h"
#include "types.h"

#ifndef CAPTURES_DIR
#define CAPTURES_DIR "test/captures"
#endif

namespace {

using atlantic_v5::BusIo;
using atlantic_v5::Channel;
using atlantic_v5::Frame;
using atlantic_v5::Relay;
using atlantic_v5::RelayPolicy;

// Host-only BusIo double: reads are chunks queued with an arrival timestamp, only
// handed back once the mock clock (set_now) reaches that timestamp. Writes and
// flush_input() calls are recorded for assertions.
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

  void write(const uint8_t *src, size_t len) override {
    std::vector<uint8_t> bytes(src, src + len);
    writes_.push_back(bytes);
    if (order_log_)
      order_log_->push_back(bytes);
  }

  void flush_input() override { flush_count_++; }
  uint32_t now_us() override { return now_; }
  void set_now(uint32_t t) { now_ = t; }

  std::vector<std::vector<uint8_t>> writes_;
  uint32_t flush_count_ = 0;
  std::vector<std::vector<uint8_t>> *order_log_ = nullptr;

 private:
  struct Chunk {
    uint32_t t_us;
    std::vector<uint8_t> bytes;
  };
  std::deque<Chunk> pending_;
  uint32_t now_ = 0;
};

std::vector<uint8_t> parse_hex(const std::string &hex) {
  std::vector<uint8_t> out;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2)
    out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  return out;
}

Channel parse_channel(const std::string &s) {
  if (s == "hmi")
    return Channel::HMI;
  if (s == "main")
    return Channel::MAIN;
  return Channel::BUS;
}

struct Row {
  uint32_t t_us;
  Channel channel;
  std::vector<uint8_t> bytes;
};

std::vector<Row> load_capture(const std::string &path) {
  std::vector<Row> rows;
  std::ifstream in(path);
  CHECK(in.is_open());
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    std::stringstream ss(line);
    std::string ts, channel, hex;
    std::getline(ss, ts, ',');
    std::getline(ss, channel, ',');
    std::getline(ss, hex, ',');
    rows.push_back({static_cast<uint32_t>(std::stoul(ts)), parse_channel(channel), parse_hex(hex)});
  }
  return rows;
}

// Recomputes a trailing little-endian CRC-16/MODBUS over frame[0..len-2), matching
// Frame::replace_payload, so expectations don't rely on a hand-computed magic number.
void append_crc(std::vector<uint8_t> &frame) {
  uint16_t crc = atlantic_v5::crc16_modbus(frame.data(), frame.size() - 2);
  frame[frame.size() - 2] = static_cast<uint8_t>(crc & 0xFF);
  frame[frame.size() - 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
}

}  // namespace

int main() {
  // --- Clean replay, PASSTHROUGH: every input frame forwarded exactly once, in
  // order, byte-identical on the opposite side. ---
  {
    auto rows = load_capture(std::string(CAPTURES_DIR) + "/synthetic_dual_bus_basic.csv");
    CHECK(rows.size() >= 14);

    MockBusIo hmi_io, main_io;
    std::vector<std::vector<uint8_t>> forwarded_in_order;
    hmi_io.order_log_ = &forwarded_in_order;
    main_io.order_log_ = &forwarded_in_order;

    RelayPolicy policy;  // defaults to PASSTHROUGH
    Relay relay(hmi_io, main_io, policy);

    for (const auto &row : rows) {
      CHECK(row.channel == Channel::HMI || row.channel == Channel::MAIN);
      (row.channel == Channel::HMI ? hmi_io : main_io).queue(row.t_us, row.bytes);
      hmi_io.set_now(row.t_us);
      main_io.set_now(row.t_us);
      relay.poll(row.t_us);
    }

    CHECK(relay.stats().frames_relayed == rows.size());
    CHECK(relay.stats().rewrites_applied == 0);
    CHECK(forwarded_in_order.size() == rows.size());
    for (size_t i = 0; i < rows.size(); i++)
      CHECK(forwarded_in_order[i] == rows[i].bytes);

    // No CRC errors or resyncs on a clean capture, on either side's assembler.
    CHECK(relay.hmi_stats().crc_errors == 0 && relay.main_stats().crc_errors == 0);
    CHECK(relay.hmi_stats().resyncs == 0 && relay.main_stats().resyncs == 0);

    // Forwarding latency budget (plan 3.6.2): <= 1ms excluding fixed transmit time.
    CHECK(relay.stats().latency_samples == rows.size());
    CHECK(relay.stats().latency_max_us <= 1000);
  }

  // --- Rewrite hook (plan 2.8): the input-status frame's I2/I1 bytes are replaced
  // per the active control mode, byte 2 (heating status) is always copied through,
  // and the CRC is recomputed. Everything else about the frame is untouched. ---
  {
    auto rows = load_capture(std::string(CAPTURES_DIR) + "/synthetic_dual_bus_basic.csv");

    MockBusIo hmi_io, main_io;
    RelayPolicy policy;
    policy.set_control_mode(atlantic_v5::ControlMode::BOOST);  // I2=1, I1=1
    Relay relay(hmi_io, main_io, policy);

    for (const auto &row : rows) {
      (row.channel == Channel::HMI ? hmi_io : main_io).queue(row.t_us, row.bytes);
      hmi_io.set_now(row.t_us);
      main_io.set_now(row.t_us);
      relay.poll(row.t_us);
    }

    CHECK(relay.stats().rewrites_applied == 1);

    // The capture's control frame: header+len 0164FF1403 03, original payload
    // 00 01 01 (I2=0,I1=1,heating=01). BOOST must yield I2=1,I1=1, heating
    // untouched, CRC recomputed over the new bytes.
    std::vector<uint8_t> expected = {0x01, 0x64, 0xFF, 0x14, 0x03, 0x03, 0x01, 0x01, 0x01, 0, 0};
    append_crc(expected);

    bool found = false;
    for (const auto &w : hmi_io.writes_) {
      if (w.size() >= 5 && w[0] == 0x01 && w[1] == 0x64 && w[2] == 0xFF && w[3] == 0x14 && w[4] == 0x03) {
        CHECK(w == expected);
        found = true;
      }
    }
    CHECK(found);
  }

  // --- Ticket 09: changing the control_mode select (RelayPolicy::set_control_mode,
  // wired from AtlanticV5Select::control()) changes the bytes the rewrite hook
  // emits, exactly per plan 2.8's table — the host-side stand-in for the
  // hardware loopback test this ticket's acceptance box refers to. Byte 2
  // (heating status, 0x01 in the capture) is always copied through regardless
  // of mode, and passthrough must leave the frame - and the rewrite counter -
  // untouched. ---
  {
    struct Case {
      atlantic_v5::ControlMode mode;
      uint8_t i2, i1;
      bool expect_rewrite;
    };
    const Case cases[] = {
        {atlantic_v5::ControlMode::PASSTHROUGH, 0x00, 0x01, false},  // original bytes, untouched
        {atlantic_v5::ControlMode::NORMAL, 0x00, 0x00, true},
        {atlantic_v5::ControlMode::EAGER, 0x00, 0x01, true},
        {atlantic_v5::ControlMode::OFF, 0x01, 0x00, true},
        {atlantic_v5::ControlMode::BOOST, 0x01, 0x01, true},
    };

    for (const auto &c : cases) {
      auto rows = load_capture(std::string(CAPTURES_DIR) + "/synthetic_dual_bus_basic.csv");
      MockBusIo hmi_io, main_io;
      RelayPolicy policy;
      policy.set_control_mode(c.mode);
      Relay relay(hmi_io, main_io, policy);

      for (const auto &row : rows) {
        (row.channel == Channel::HMI ? hmi_io : main_io).queue(row.t_us, row.bytes);
        hmi_io.set_now(row.t_us);
        main_io.set_now(row.t_us);
        relay.poll(row.t_us);
      }

      CHECK(relay.stats().rewrites_applied == (c.expect_rewrite ? 1u : 0u));

      std::vector<uint8_t> expected = {0x01, 0x64, 0xFF, 0x14, 0x03, 0x03, c.i2, c.i1, 0x01, 0, 0};
      append_crc(expected);

      bool found = false;
      for (const auto &w : hmi_io.writes_) {
        if (w.size() >= 5 && w[0] == 0x01 && w[1] == 0x64 && w[2] == 0xFF && w[3] == 0x14 && w[4] == 0x03) {
          CHECK(w == expected);
          found = true;
        }
      }
      CHECK(found);
    }
  }

  // --- reset_latency_stats (plan 3.8: relay_latency_avg_us/max_us are "reset
  // on read"): zeroes only the latency fields, leaving the plain cumulative
  // counters (frames_relayed, rewrites_applied, echo_bytes) untouched. ---
  {
    auto rows = load_capture(std::string(CAPTURES_DIR) + "/synthetic_dual_bus_basic.csv");
    MockBusIo hmi_io, main_io;
    RelayPolicy policy;
    Relay relay(hmi_io, main_io, policy);

    for (const auto &row : rows) {
      (row.channel == Channel::HMI ? hmi_io : main_io).queue(row.t_us, row.bytes);
      hmi_io.set_now(row.t_us);
      main_io.set_now(row.t_us);
      relay.poll(row.t_us);
    }
    CHECK(relay.stats().latency_samples == rows.size());
    uint32_t frames_relayed_before = relay.stats().frames_relayed;

    relay.reset_latency_stats();

    CHECK(relay.stats().latency_samples == 0);
    CHECK(relay.stats().latency_max_us == 0);
    CHECK(relay.stats().latency_total_us == 0);
    CHECK(relay.stats().frames_relayed == frames_relayed_before);  // untouched
  }

  // --- Fail-safe forwarding (plan 3.9 #2 / 2.5.1): a frame with a bad CRC is
  // still forwarded raw and unmodified — the rewrite hook must never touch it,
  // and the error is counted, not silently dropped. ---
  {
    auto rows = load_capture(std::string(CAPTURES_DIR) + "/synthetic_dual_bus_basic.csv");
    // Corrupt one payload byte of the MAIN response to 0164FEB006 (row 1), breaking its CRC.
    for (auto &row : rows) {
      if (row.channel == Channel::MAIN && row.bytes.size() > 6 && row.bytes[0] == 0x01 && row.bytes[2] == 0xFE &&
          row.bytes[3] == 0xB0) {
        row.bytes[6] ^= 0xFF;
        break;
      }
    }

    MockBusIo hmi_io, main_io;
    RelayPolicy policy;
    policy.set_control_mode(atlantic_v5::ControlMode::BOOST);  // even active, must not touch this header
    Relay relay(hmi_io, main_io, policy);

    for (const auto &row : rows) {
      (row.channel == Channel::HMI ? hmi_io : main_io).queue(row.t_us, row.bytes);
      hmi_io.set_now(row.t_us);
      main_io.set_now(row.t_us);
      relay.poll(row.t_us);
    }

    CHECK(relay.main_stats().crc_errors == 1);
    bool found_corrupted = false;
    for (const auto &row : rows) {
      if (row.channel != Channel::MAIN)
        continue;
      for (const auto &w : hmi_io.writes_) {
        if (w == row.bytes)
          found_corrupted = true;
      }
    }
    CHECK(found_corrupted);
  }

  // --- Echo accounting (plan 3.5.3): bytes arriving on a side within
  // echo_drain_us of a write to that same side are our own echo — discarded,
  // counted, and never mistaken for a new frame to relay back. ---
  {
    MockBusIo hmi_io, main_io;
    RelayPolicy policy;
    Relay relay(hmi_io, main_io, policy, Relay::Config{/*silence_us=*/4000, /*echo_drain_us=*/200});

    // A clean payload-less HMI request (header 0164006401, plan 2.7 firmware version).
    std::vector<uint8_t> req = {0x01, 0x64, 0x00, 0x64, 0x01, 0, 0};
    append_crc(req);

    hmi_io.queue(1000, req);
    hmi_io.set_now(1000);
    main_io.set_now(1000);
    relay.poll(1000);

    CHECK(relay.stats().frames_relayed == 1);
    CHECK(main_io.writes_.size() == 1);
    CHECK(main_io.writes_[0] == req);

    // The transceiver echoes our own write back into main_io's RX, well inside
    // the 200us drain window.
    main_io.queue(1050, req);
    hmi_io.set_now(1050);
    main_io.set_now(1050);
    relay.poll(1050);

    CHECK(relay.stats().echo_bytes == req.size());
    CHECK(relay.stats().frames_relayed == 1);  // unchanged: the echo was not relayed
    CHECK(main_io.writes_.size() == 1);
    CHECK(hmi_io.writes_.empty());
  }

  // --- Raw capture piggyback (plan 3.5.5 MITM piggyback): the capture sink sees
  // every chunk read from either side, tagged with the correct channel, including
  // the echoed chunk that echo accounting discards from framing. ---
  {
    auto rows = load_capture(std::string(CAPTURES_DIR) + "/synthetic_dual_bus_basic.csv");

    MockBusIo hmi_io, main_io;
    RelayPolicy policy;
    Relay relay(hmi_io, main_io, policy);

    struct Captured {
      Channel channel;
      std::vector<uint8_t> bytes;
    };
    std::vector<Captured> captured;
    relay.set_capture_sink(
        [](void *ctx, Channel channel, const uint8_t *data, size_t len, uint32_t /*t_us*/) {
          auto *out = static_cast<std::vector<Captured> *>(ctx);
          out->push_back({channel, std::vector<uint8_t>(data, data + len)});
        },
        &captured);

    for (const auto &row : rows) {
      (row.channel == Channel::HMI ? hmi_io : main_io).queue(row.t_us, row.bytes);
      hmi_io.set_now(row.t_us);
      main_io.set_now(row.t_us);
      relay.poll(row.t_us);
    }

    CHECK(captured.size() == rows.size());
    for (size_t i = 0; i < rows.size(); i++) {
      CHECK(captured[i].channel == rows[i].channel);
      CHECK(captured[i].bytes == rows[i].bytes);
    }
  }

  // --- Cross-thread handoff (plan 3.6.5): the frame sink sees every completed
  // frame, tagged with its origin channel and reflecting any rewrite, and always
  // fires strictly after the corresponding write to the opposite side ("forward
  // first, enqueue second", plan 3.9 #1). ---
  {
    auto rows = load_capture(std::string(CAPTURES_DIR) + "/synthetic_dual_bus_basic.csv");

    MockBusIo hmi_io, main_io;
    RelayPolicy policy;
    policy.set_control_mode(atlantic_v5::ControlMode::BOOST);
    Relay relay(hmi_io, main_io, policy);

    struct Event {
      Channel channel;
      std::vector<uint8_t> bytes;
      size_t writes_so_far;
    };
    std::vector<Event> events;
    struct Ctx {
      std::vector<Event> *events;
      MockBusIo *hmi_io;
      MockBusIo *main_io;
    } ctx{&events, &hmi_io, &main_io};
    relay.set_frame_sink(
        [](void *raw_ctx, Channel channel, const Frame &f, uint32_t /*t_us*/) {
          auto *c = static_cast<Ctx *>(raw_ctx);
          size_t writes = c->hmi_io->writes_.size() + c->main_io->writes_.size();
          c->events->push_back({channel, std::vector<uint8_t>(f.raw(), f.raw() + f.raw_len()), writes});
        },
        &ctx);

    for (const auto &row : rows) {
      (row.channel == Channel::HMI ? hmi_io : main_io).queue(row.t_us, row.bytes);
      hmi_io.set_now(row.t_us);
      main_io.set_now(row.t_us);
      relay.poll(row.t_us);
    }

    CHECK(events.size() == rows.size());
    for (size_t i = 0; i < rows.size(); i++) {
      // The write to the opposite side already happened by the time the sink runs.
      CHECK(events[i].writes_so_far == i + 1);
      CHECK(events[i].channel == rows[i].channel);
    }
    // The BOOST-rewritten control frame's event carries the rewritten bytes, not
    // the original ones. The header 0164FF1403 also appears as HMI's payload-less
    // (7-byte) poll request, which RelayPolicy never touches (payload_len() != 3)
    // — only the payload-bearing (11-byte) MAIN response is rewritten.
    bool found_rewritten_event = false;
    for (size_t i = 0; i < rows.size(); i++) {
      if (rows[i].bytes.size() > 7 && rows[i].bytes[0] == 0x01 && rows[i].bytes[1] == 0x64 &&
          rows[i].bytes[2] == 0xFF && rows[i].bytes[3] == 0x14 && rows[i].bytes[4] == 0x03) {
        CHECK(events[i].bytes != rows[i].bytes);
        found_rewritten_event = true;
      }
    }
    CHECK(found_rewritten_event);
  }

  TEST_MAIN_RETURN();
}
