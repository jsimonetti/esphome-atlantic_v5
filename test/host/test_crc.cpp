// M1 core test (ticket 03): crc16_modbus, Frame accessors, replace_payload round-trip.
// Host-only: reads test/captures/synthetic_*.csv, no ESP headers.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "crc16.h"
#include "frame.h"
#include "types.h"
#include "test_harness.h"

#ifndef CAPTURES_DIR
#define CAPTURES_DIR "test/captures"
#endif

namespace {

// Independent CRC-16/MODBUS implementation (reflect around a non-reflected,
// MSB-first poly 0x8005), mirroring the cross-check gen_synthetic_captures.py
// already applies before writing the fixtures out. Used here to verify
// core/crc16.cpp against a second, differently-shaped implementation rather
// than against itself.
uint8_t reflect8(uint8_t value) {
  uint8_t result = 0;
  for (int i = 0; i < 8; i++)
    if (value & (1 << i))
      result |= 1 << (7 - i);
  return result;
}

uint16_t reflect16(uint16_t value) {
  uint16_t result = 0;
  for (int i = 0; i < 16; i++)
    if (value & (1 << i))
      result |= 1 << (15 - i);
  return result;
}

uint16_t crc16_modbus_msb_first(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= static_cast<uint16_t>(reflect8(data[i])) << 8;
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x8005) : static_cast<uint16_t>(crc << 1);
  }
  return reflect16(crc);
}

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
    rows.push_back({parse_channel(channel), parse_hex(hex)});
  }
  return rows;
}

// Frame trusts its input to already delimit one candidate frame (frame.h); this
// mirrors the length rules of plan 2.3/2.5 just enough to tell a genuinely
// complete row (payload-less, or a payload frame with all its bytes present)
// apart from the deliberately truncated fixture row, which is M2's concern.
bool is_complete_frame(const std::vector<uint8_t> &bytes) {
  if (bytes.size() == atlantic_v5::HEADER_LEN + 2)
    return true;  // payload-less: header + CRC
  if (bytes.size() > atlantic_v5::HEADER_LEN + 2) {
    size_t n = bytes[atlantic_v5::HEADER_LEN];
    size_t expected = atlantic_v5::HEADER_LEN + 1 + n + 2;
    return bytes.size() == expected;
  }
  return false;
}

}  // namespace

int main() {
  std::vector<Row> rows;
  for (const char *name : {"synthetic_dual_bus_basic.csv", "synthetic_single_bus_interleaved.csv"}) {
    auto capture_rows = load_capture(std::string(CAPTURES_DIR) + "/" + name);
    rows.insert(rows.end(), capture_rows.begin(), capture_rows.end());
  }

  int complete_frames = 0;
  int payload_less_frames = 0;

  for (const auto &row : rows) {
    if (!is_complete_frame(row.bytes))
      continue;  // deliberately truncated fixture row (M2 territory), skip
    complete_frames++;

    atlantic_v5::Frame f(row.channel, row.bytes.data(), static_cast<uint8_t>(row.bytes.size()));

    // Cross-check core/crc16.cpp against an independently-shaped implementation.
    uint16_t computed = atlantic_v5::crc16_modbus(row.bytes.data(), row.bytes.size() - 2);
    uint16_t computed_alt = crc16_modbus_msb_first(row.bytes.data(), row.bytes.size() - 2);
    CHECK(computed == computed_alt);
    uint16_t stored = static_cast<uint16_t>(row.bytes[row.bytes.size() - 2]) |
                       (static_cast<uint16_t>(row.bytes[row.bytes.size() - 1]) << 8);
    bool expected_valid = computed == stored;
    CHECK(f.crc_valid() == expected_valid);

    if (!f.has_payload()) {
      payload_less_frames++;
      CHECK(f.raw_len() == atlantic_v5::HEADER_LEN + 2);
      CHECK(f.payload_len() == 0);
    } else {
      CHECK(f.payload_len() == row.bytes[atlantic_v5::HEADER_LEN]);
    }
  }

  CHECK(complete_frames >= 20);
  CHECK(payload_less_frames >= 3);

  // replace_payload round-trip: modify, recompute, verify (ticket 03 checklist).
  // 0x0164FEB006 water-temperature response, 12-byte payload (plan 2.7).
  const uint8_t original[] = {0x01, 0x64, 0xFE, 0xB0, 0x06, 0x0C, 0x11, 0xD7, 0x14,
                               0x6E, 0x07, 0x3A, 0x02, 0x0D, 0x01, 0xFE, 0x01, 0xF4, 0x90, 0x2D};
  atlantic_v5::Frame f(atlantic_v5::Channel::MAIN, original, sizeof(original));
  CHECK(f.crc_valid());
  CHECK(!f.modified());

  uint8_t new_payload[12];
  std::memcpy(new_payload, f.payload(), sizeof(new_payload));
  new_payload[0] = 0x12;
  new_payload[1] = 0x34;
  f.replace_payload(new_payload);

  CHECK(f.modified());
  CHECK(f.crc_valid());
  CHECK(std::memcmp(f.payload(), new_payload, sizeof(new_payload)) == 0);
  // Header is untouched by the rewrite.
  CHECK(std::memcmp(f.raw(), original, atlantic_v5::HEADER_LEN) == 0);
  // CRC actually changed (payload did), proving recompute ran rather than a stale copy.
  CHECK(std::memcmp(f.raw() + f.raw_len() - 2, original + sizeof(original) - 2, 2) != 0);

  TEST_MAIN_RETURN();
}
