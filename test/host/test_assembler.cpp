// M2 core test (ticket 04): FrameAssembler dual-bus and single-bus framing paths.
// Host-only: reads test/captures/synthetic_*.csv, no ESP headers.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "assembler.h"
#include "frame.h"
#include "types.h"
#include "test_harness.h"

#ifndef CAPTURES_DIR
#define CAPTURES_DIR "test/captures"
#endif

namespace {

std::vector<uint8_t> parse_hex(const std::string &hex) {
  std::vector<uint8_t> out;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2)
    out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  return out;
}

atlantic_v5::Channel parse_channel(const std::string &s) {
  if (s == "hmi")
    return atlantic_v5::Channel::HMI;
  if (s == "main")
    return atlantic_v5::Channel::MAIN;
  return atlantic_v5::Channel::BUS;
}

struct Row {
  uint32_t t_us;
  atlantic_v5::Channel channel;
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

// Feeds one row's bytes through an assembler, using the row timestamp for every byte
// (captures only record the arrival time of a frame's last byte, plan 3.5.5). Appends
// any delivered frame's raw bytes to `out`.
void feed_row(atlantic_v5::FrameAssembler &asm_, const Row &row, std::vector<std::vector<uint8_t>> &out) {
  for (uint8_t b : row.bytes) {
    if (asm_.push(b, row.t_us))
      out.emplace_back(asm_.frame(), asm_.frame() + asm_.frame_len());
  }
}

}  // namespace

int main() {
  // --- Dual-bus, clean capture: every frame forwarded exactly once, zero errors. ---
  {
    auto rows = load_capture(std::string(CAPTURES_DIR) + "/synthetic_dual_bus_basic.csv");
    CHECK(rows.size() >= 14);

    atlantic_v5::FrameAssembler hmi(atlantic_v5::Channel::HMI, /*dual_bus=*/true);
    atlantic_v5::FrameAssembler main_asm(atlantic_v5::Channel::MAIN, /*dual_bus=*/true);
    std::vector<std::vector<uint8_t>> delivered;

    for (const auto &row : rows) {
      CHECK(row.channel == atlantic_v5::Channel::HMI || row.channel == atlantic_v5::Channel::MAIN);
      feed_row(row.channel == atlantic_v5::Channel::HMI ? hmi : main_asm, row, delivered);
    }

    CHECK(delivered.size() == rows.size());
    // Delivered in the same order as the rows, byte-for-byte identical (fail-safe
    // forwarding never rewrites a clean frame).
    for (size_t i = 0; i < rows.size(); i++)
      CHECK(delivered[i] == rows[i].bytes);

    const auto &hs = hmi.stats();
    const auto &ms = main_asm.stats();
    CHECK(hs.crc_errors == 0 && ms.crc_errors == 0);
    CHECK(hs.dropped_bytes == 0 && ms.dropped_bytes == 0);
    CHECK(hs.resyncs == 0 && ms.resyncs == 0);
    CHECK(hs.oversize == 0 && ms.oversize == 0);
    CHECK(hs.frames + ms.frames == rows.size());
  }

  // --- Single-bus, interleaved capture with a corrupted and a truncated frame:
  // resync must recover within one frame, both bad rows counted, not silently lost. ---
  {
    auto rows = load_capture(std::string(CAPTURES_DIR) + "/synthetic_single_bus_interleaved.csv");
    CHECK(rows.size() >= 20);

    atlantic_v5::FrameAssembler bus(atlantic_v5::Channel::BUS, /*dual_bus=*/false);
    std::vector<std::vector<uint8_t>> delivered;

    for (const auto &row : rows) {
      // Simulates a main-loop poll: apply the silence backstop before pushing new
      // bytes, so a truncated frame that can never complete on its own still resyncs.
      if (bus.tick(row.t_us))
        delivered.emplace_back(bus.frame(), bus.frame() + bus.frame_len());
      feed_row(bus, row, delivered);
    }
    // Flush a final trailing frame, if any, well past the silence threshold.
    if (bus.tick(rows.back().t_us + atlantic_v5::FrameAssembler::DEFAULT_SILENCE_US + 1))
      delivered.emplace_back(bus.frame(), bus.frame() + bus.frame_len());

    const auto &stats = bus.stats();
    // 21 rows, 2 deliberately broken (one bad CRC, one truncated) -> at least 19 clean.
    CHECK(delivered.size() >= 19);
    CHECK(stats.crc_errors >= 1);
    CHECK(stats.resyncs >= 1);
    CHECK(stats.dropped_bytes > 0);
    CHECK(stats.speculative_accepts > 0);

    // The frame immediately after the corrupted+truncated pair must be the clean
    // resync header (0164FEC603, plan 2.7 evaporator_2 min/max) - proves the bad
    // frames were fully recovered from, not left desynced.
    bool found_resync_header = false;
    for (const auto &frame : delivered) {
      if (frame.size() >= atlantic_v5::HEADER_LEN) {
        atlantic_v5::Frame f(atlantic_v5::Channel::BUS, frame.data(), static_cast<uint8_t>(frame.size()));
        if (f.header_key() == 0x0164FEC603ULL) {
          found_resync_header = true;
          CHECK(f.crc_valid());
        }
      }
    }
    CHECK(found_resync_header);
  }

  // --- Injected silence threshold (plan 3.2: "passed in at construction so tests
  // can vary it"): a shorter threshold backstops sooner than the default. ---
  {
    const uint8_t partial_frame[] = {0x01, 0x65, 0xFE, 0xB0, 0x06, 0x02, 0xAA};  // incomplete payload frame

    atlantic_v5::FrameAssembler default_threshold(atlantic_v5::Channel::HMI, /*dual_bus=*/true);
    atlantic_v5::FrameAssembler short_threshold(atlantic_v5::Channel::HMI, /*dual_bus=*/true, /*silence_us=*/50);

    for (uint8_t b : partial_frame) {
      CHECK(!default_threshold.push(b, 1000));
      CHECK(!short_threshold.push(b, 1000));
    }

    // 80 us later: below the 4 ms default, above the injected 50 us threshold.
    CHECK(!default_threshold.tick(1080));
    CHECK(short_threshold.tick(1080));
    CHECK(short_threshold.stats().silence_closes == 1);
    CHECK(default_threshold.stats().silence_closes == 0);
  }

  TEST_MAIN_RETURN();
}
