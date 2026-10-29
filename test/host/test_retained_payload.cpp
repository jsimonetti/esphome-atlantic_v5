// Core test: the retained payload's two sections (init-cadence values and the
// cold water temperature) and what it refuses to decode.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "catalog.h"
#include "crc16.h"
#include "init_values.h"
#include "retained_payload.h"
#include "retention.h"
#include "test_harness.h"

using atlantic_v5::DecodedValue;
using atlantic_v5::InitValueStore;
using atlantic_v5::RetainedPayload;
using atlantic_v5::RetentionBlock;

namespace {

DecodedValue float_value(uint16_t id, float f) {
  DecodedValue v{};
  v.kind = DecodedValue::Kind::FLOAT;
  v.id = id;
  v.f = f;
  return v;
}

DecodedValue text_value(uint16_t id, const char *s) {
  DecodedValue v{};
  v.kind = DecodedValue::Kind::TEXT;
  v.id = id;
  std::strncpy(v.text, s, sizeof(v.text) - 1);
  return v;
}

size_t held(const InitValueStore &s) {
  size_t n = 0;
  s.each([](void *ctx, const DecodedValue &) { ++*static_cast<size_t *>(ctx); }, &n);
  return n;
}

void fill(InitValueStore &s) {
  s.observe(text_value(atlantic_v5::ENT_APPLIANCE_SERIAL, "100626280202173"));
  s.observe(float_value(atlantic_v5::ENT_SETPOINT_MAX, 62.0f));
}

std::vector<uint8_t> encode(const InitValueStore &s, const float *cold) {
  uint8_t buf[RetainedPayload::MAX_ENCODED];
  const uint16_t len = RetainedPayload::encode(s, cold, buf, sizeof(buf));
  return std::vector<uint8_t>(buf, buf + len);
}

bool decode(const std::vector<uint8_t> &b, InitValueStore &s, bool &has_cold, float &cold) {
  return RetainedPayload::decode(b.data(), static_cast<uint16_t>(b.size()), s, has_cold, cold);
}

void test_cold_only_round_trips() {
  InitValueStore src;
  const float cold = 12.5f;
  const auto bytes = encode(src, &cold);
  CHECK(!bytes.empty());

  InitValueStore dst;
  bool has_cold = false;
  float got = 0;
  CHECK(decode(bytes, dst, has_cold, got));
  CHECK(has_cold);
  CHECK(got == 12.5f);
  CHECK(held(dst) == 0);
}

void test_init_only_round_trips_without_cold() {
  InitValueStore src;
  fill(src);
  const auto bytes = encode(src, nullptr);
  CHECK(!bytes.empty());

  InitValueStore dst;
  bool has_cold = true;
  float got = 0;
  CHECK(decode(bytes, dst, has_cold, got));
  CHECK(!has_cold);
  CHECK(held(dst) == 2);
}

void test_both_sections_round_trip() {
  InitValueStore src;
  fill(src);
  const float cold = 8.0f;
  const auto bytes = encode(src, &cold);

  InitValueStore dst;
  bool has_cold = false;
  float got = 0;
  CHECK(decode(bytes, dst, has_cold, got));
  CHECK(has_cold && got == 8.0f);
  CHECK(held(dst) == 2);
}

void test_nothing_to_retain_encodes_nothing() {
  InitValueStore src;
  uint8_t buf[RetainedPayload::MAX_ENCODED];
  CHECK(RetainedPayload::encode(src, nullptr, buf, sizeof(buf)) == 0);
}

void test_full_init_set_and_cold_fit_the_block() {
  CHECK(RetainedPayload::MAX_ENCODED <= RetentionBlock::CAPACITY);
}

void test_malformed_payloads_are_rejected() {
  InitValueStore src;
  fill(src);
  const float cold = 8.0f;
  const auto good = encode(src, &cold);

  InitValueStore dst;
  bool has_cold = false;
  float got = 0;

  CHECK(!RetainedPayload::decode(nullptr, 0, dst, has_cold, got));

  // Every strict prefix is rejected.
  for (size_t len = 1; len < good.size(); len++) {
    CHECK(!RetainedPayload::decode(good.data(), static_cast<uint16_t>(len), dst, has_cold, got));
    CHECK(held(dst) == 0);
  }

  auto trailing = good;
  trailing.push_back(0);
  CHECK(!decode(trailing, dst, has_cold, got));

  // Unknown section flag.
  auto flags = good;
  flags[0] |= 0x80;
  CHECK(!decode(flags, dst, has_cold, got));

  // No sections at all is not a payload.
  CHECK(!decode({0x00}, dst, has_cold, got));
}

void test_out_of_range_or_non_finite_cold_is_rejected() {
  InitValueStore src;
  InitValueStore dst;
  bool has_cold = false;
  float got = 0;
  for (float bad : {-0.5f, 30.5f, std::nanf(""), INFINITY}) {
    const auto bytes = encode(src, &bad);
    CHECK(!decode(bytes, dst, has_cold, got));
  }
  for (float edge : {0.0f, 30.0f}) {
    const auto bytes = encode(src, &edge);
    CHECK(decode(bytes, dst, has_cold, got));
  }
}

// A bad cold section must not leave the init values half-restored.
void test_bad_cold_section_leaves_init_store_empty() {
  InitValueStore src;
  fill(src);
  const float cold = 8.0f;
  auto bytes = encode(src, &cold);
  // The cold float is the last four bytes: make it NaN.
  const float nan = std::nanf("");
  std::memcpy(bytes.data() + bytes.size() - 4, &nan, 4);

  InitValueStore dst;
  bool has_cold = false;
  float got = 0;
  CHECK(!decode(bytes, dst, has_cold, got));
  CHECK(held(dst) == 0);
}

// A block written by the previous layout (init-cadence encoding as the whole
// payload) is refused by the guard before any decoder sees it.
void test_previous_layout_block_is_rejected() {
  CHECK(RetentionBlock::LAYOUT_VERSION >= 3);

  RetentionBlock block;
  std::memset(&block, 0xA5, sizeof(block));
  InitValueStore src;
  fill(src);
  const auto bytes = encode(src, nullptr);
  CHECK(block.store(bytes.data(), static_cast<uint16_t>(bytes.size())));

  uint8_t out[RetentionBlock::CAPACITY];
  uint16_t out_len = 0;
  CHECK(block.load(out, sizeof(out), out_len));

  // Offsets: magic 0-3, crc 4-5, layout version 6-7, payload length 8-9. Patch
  // the version and re-seal the CRC, so only the version check can refuse it.
  auto *raw = reinterpret_cast<uint8_t *>(&block);
  auto reseal = [&] {
    const uint16_t crc = atlantic_v5::crc16_modbus(raw + 6, 4 + out_len);
    raw[4] = static_cast<uint8_t>(crc);
    raw[5] = static_cast<uint8_t>(crc >> 8);
  };
  reseal();
  CHECK(block.load(out, sizeof(out), out_len));  // control: resealing alone is accepted
  raw[6] = 2;
  raw[7] = 0;
  reseal();
  CHECK(!block.load(out, sizeof(out), out_len));
}

}  // namespace

int main() {
  test_cold_only_round_trips();
  test_init_only_round_trips_without_cold();
  test_both_sections_round_trip();
  test_nothing_to_retain_encodes_nothing();
  test_full_init_set_and_cold_fit_the_block();
  test_malformed_payloads_are_rejected();
  test_out_of_range_or_non_finite_cold_is_rejected();
  test_bad_cold_section_leaves_init_store_empty();
  test_previous_layout_block_is_rejected();
  TEST_MAIN_RETURN();
}
