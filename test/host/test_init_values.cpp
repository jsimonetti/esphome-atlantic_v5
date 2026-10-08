// Core test: the init-value store's encode/decode and what it refuses to
// decode. The retained bytes come from the same uninitialised SRAM the
// RetentionBlock guard exists for, so every rejection here is a value that
// would otherwise reach Home Assistant as a fact about the appliance.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "catalog.h"
#include "init_values.h"
#include "retention.h"
#include "test_harness.h"

using atlantic_v5::DecodedValue;
using atlantic_v5::InitValueStore;
using atlantic_v5::RetentionBlock;

namespace {

using Observation = InitValueStore::Observation;

DecodedValue text_value(uint16_t id, const char *s) {
  DecodedValue v{};
  v.kind = DecodedValue::Kind::TEXT;
  v.id = id;
  std::strncpy(v.text, s, sizeof(v.text) - 1);
  return v;
}

DecodedValue float_value(uint16_t id, float f) {
  DecodedValue v{};
  v.kind = DecodedValue::Kind::FLOAT;
  v.id = id;
  v.f = f;
  return v;
}

std::vector<DecodedValue> collect(const InitValueStore &s) {
  std::vector<DecodedValue> out;
  s.each([](void *ctx, const DecodedValue &v) { static_cast<std::vector<DecodedValue> *>(ctx)->push_back(v); },
         &out);
  return out;
}

// The full nine, as the appliance states them: seven identity strings plus the
// two setpoint bounds.
std::vector<DecodedValue> all_nine() {
  return {
      text_value(atlantic_v5::ENT_CONTROLLER_FIRMWARE_VERSION, "2.9"),
      text_value(atlantic_v5::ENT_APPLIANCE_SERIAL, "100626280202173"),
      text_value(atlantic_v5::ENT_PRODUCT_CODE, "100088611500"),
      text_value(atlantic_v5::ENT_POWER_BOARD_VERSION, "1.6.1"),
      text_value(atlantic_v5::ENT_CONTROLLER_MODEL, "600U12345678"),
      float_value(atlantic_v5::ENT_SETPOINT_MIN, 50.0f),
      float_value(atlantic_v5::ENT_SETPOINT_MAX, 62.0f),
      text_value(atlantic_v5::ENT_HMI_FIRMWARE_VERSION, "2.7.3"),
      text_value(atlantic_v5::ENT_HMI_MODEL, "600U87654321"),
  };
}

void fill(InitValueStore &s) {
  for (const auto &v : all_nine())
    CHECK(s.observe(v) == Observation::CHANGED);
}

void test_round_trip_of_the_whole_set() {
  InitValueStore src;
  fill(src);

  uint8_t buf[InitValueStore::MAX_ENCODED];
  const uint16_t len = src.encode(buf, sizeof(buf));
  CHECK(len > 0);

  InitValueStore dst;
  CHECK(dst.decode(buf, len));

  const auto expected = all_nine();
  const auto got = collect(dst);
  CHECK(got.size() == expected.size());
  if (got.size() != expected.size())
    return;
  for (size_t i = 0; i < got.size(); i++) {
    CHECK(got[i].id == expected[i].id);
    CHECK(got[i].kind == expected[i].kind);
    if (expected[i].kind == DecodedValue::Kind::TEXT)
      CHECK(std::string(got[i].text) == std::string(expected[i].text));
    else
      CHECK(got[i].f == expected[i].f);
  }
}

// The dedupe the republish rule rests on: a live observation that matches what
// was restored must not look like news.
void test_observe_reports_only_real_changes() {
  InitValueStore s;
  const auto serial = text_value(atlantic_v5::ENT_APPLIANCE_SERIAL, "100626280202173");
  CHECK(s.observe(serial) == Observation::CHANGED);
  CHECK(s.observe(serial) == Observation::UNCHANGED);
  CHECK(s.observe(text_value(atlantic_v5::ENT_APPLIANCE_SERIAL, "100626280202174")) == Observation::CHANGED);

  const auto bound = float_value(atlantic_v5::ENT_SETPOINT_MAX, 62.0f);
  CHECK(s.observe(bound) == Observation::CHANGED);
  CHECK(s.observe(bound) == Observation::UNCHANGED);
  CHECK(s.observe(float_value(atlantic_v5::ENT_SETPOINT_MAX, 60.0f)) == Observation::CHANGED);
}

// A value that differs only in kind is a different value: otherwise a store
// holding text could report a float observation as already published.
void test_same_id_different_kind_is_a_change() {
  InitValueStore s;
  CHECK(s.observe(float_value(atlantic_v5::ENT_SETPOINT_MIN, 0.0f)) == Observation::CHANGED);
  DecodedValue as_text = text_value(atlantic_v5::ENT_SETPOINT_MIN, "");
  CHECK(s.observe(as_text) == Observation::CHANGED);
}

void test_non_init_cadence_values_are_ignored() {
  InitValueStore s;
  CHECK(s.observe(float_value(atlantic_v5::ENT_WATER_TEMPERATURE, 49.16f)) == Observation::NOT_INIT_CADENCE);
  CHECK(s.observe(float_value(atlantic_v5::ENT_VALID_FRAMES, 12.0f)) == Observation::NOT_INIT_CADENCE);
  CHECK(collect(s).empty());

  uint8_t buf[InitValueStore::MAX_ENCODED];
  CHECK(s.encode(buf, sizeof(buf)) == 0);
}

// A restart before the first burst must retain nothing rather than an empty
// record set that would read back as nine blank entities.
void test_empty_store_encodes_nothing() {
  InitValueStore s;
  uint8_t buf[InitValueStore::MAX_ENCODED];
  CHECK(s.encode(buf, sizeof(buf)) == 0);
}

void test_partial_set_round_trips() {
  InitValueStore src;
  CHECK(src.observe(text_value(atlantic_v5::ENT_HMI_MODEL, "600U87654321")) == Observation::CHANGED);

  uint8_t buf[InitValueStore::MAX_ENCODED];
  const uint16_t len = src.encode(buf, sizeof(buf));
  CHECK(len > 0);

  InitValueStore dst;
  CHECK(dst.decode(buf, len));
  const auto got = collect(dst);
  CHECK(got.size() == 1);
  if (got.size() == 1)
    CHECK(got[0].id == atlantic_v5::ENT_HMI_MODEL);
}

void test_encode_refuses_a_buffer_it_would_overrun() {
  InitValueStore s;
  fill(s);
  uint8_t small[8];
  CHECK(s.encode(small, sizeof(small)) == 0);
  CHECK(s.encode(small, 0) == 0);
}

// Everything below is a block the guard has already let through - the magic,
// the layout version and the CRC all agree - so the only thing standing
// between these bytes and a published value is this decoder.
std::vector<uint8_t> encoded_nine() {
  InitValueStore s;
  fill(s);
  uint8_t buf[InitValueStore::MAX_ENCODED];
  const uint16_t len = s.encode(buf, sizeof(buf));
  return std::vector<uint8_t>(buf, buf + len);
}

void test_empty_input_is_rejected() {
  InitValueStore s;
  CHECK(!s.decode(nullptr, 0));
  const uint8_t zero = 0;
  CHECK(!s.decode(&zero, 1));
}

void test_truncated_input_is_rejected() {
  auto bytes = encoded_nine();
  for (size_t len = 1; len < bytes.size(); len++) {
    InitValueStore s;
    if (s.decode(bytes.data(), static_cast<uint16_t>(len))) {
      CHECK(false);  // a prefix decoded as a complete block
      break;
    }
    CHECK(collect(s).empty());
  }
}

void test_trailing_bytes_are_rejected() {
  auto bytes = encoded_nine();
  bytes.push_back(0x00);
  InitValueStore s;
  CHECK(!s.decode(bytes.data(), static_cast<uint16_t>(bytes.size())));
}

void test_record_count_above_the_set_is_rejected() {
  auto bytes = encoded_nine();
  bytes[0] = static_cast<uint8_t>(atlantic_v5::INIT_CADENCE_COUNT + 1);
  InitValueStore s;
  CHECK(!s.decode(bytes.data(), static_cast<uint16_t>(bytes.size())));
}

void test_foreign_entity_id_is_rejected() {
  auto bytes = encoded_nine();
  bytes[1] = static_cast<uint8_t>(atlantic_v5::ENT_WATER_TEMPERATURE);
  bytes[2] = 0;
  InitValueStore s;
  CHECK(!s.decode(bytes.data(), static_cast<uint16_t>(bytes.size())));
  CHECK(collect(s).empty());
}

void test_duplicate_entity_id_is_rejected() {
  InitValueStore src;
  CHECK(src.observe(text_value(atlantic_v5::ENT_HMI_MODEL, "a")) == Observation::CHANGED);
  CHECK(src.observe(text_value(atlantic_v5::ENT_APPLIANCE_SERIAL, "a")) == Observation::CHANGED);
  uint8_t buf[InitValueStore::MAX_ENCODED];
  const uint16_t len = src.encode(buf, sizeof(buf));
  CHECK(len > 0);
  // Rewrite the second record's id to repeat the first.
  buf[1 + InitValueStore::RECORD_HEADER_BYTES + 1] = buf[1];
  buf[1 + InitValueStore::RECORD_HEADER_BYTES + 2] = buf[2];
  InitValueStore dst;
  CHECK(!dst.decode(buf, len));
}

void test_unknown_kind_is_rejected() {
  auto bytes = encoded_nine();
  bytes[3] = 0x7F;
  InitValueStore s;
  CHECK(!s.decode(bytes.data(), static_cast<uint16_t>(bytes.size())));
}

void test_value_length_must_match_the_kind() {
  InitValueStore src;
  CHECK(src.observe(float_value(atlantic_v5::ENT_SETPOINT_MIN, 50.0f)) == Observation::CHANGED);
  uint8_t buf[InitValueStore::MAX_ENCODED];
  uint16_t len = src.encode(buf, sizeof(buf));
  CHECK(len > 0);
  buf[4] = 3;  // a three-byte float
  InitValueStore dst;
  CHECK(!dst.decode(buf, len));
}

// A declared text length wider than DecodedValue::text, in a buffer long
// enough to supply the bytes, so the refusal comes from the field's own bound
// rather than from running off the end of the input.
void test_oversized_text_is_rejected() {
  const uint8_t too_long = InitValueStore::MAX_VALUE_BYTES + 1;
  std::vector<uint8_t> bytes;
  bytes.push_back(1);
  bytes.push_back(static_cast<uint8_t>(atlantic_v5::ENT_HMI_MODEL));
  bytes.push_back(static_cast<uint8_t>(atlantic_v5::ENT_HMI_MODEL >> 8));
  bytes.push_back(static_cast<uint8_t>(DecodedValue::Kind::TEXT));
  bytes.push_back(too_long);
  bytes.insert(bytes.end(), too_long, 'x');

  InitValueStore s;
  CHECK(!s.decode(bytes.data(), static_cast<uint16_t>(bytes.size())));

  // The same encoding one byte narrower is accepted, which is what makes the
  // rejection above about the bound and not about the record shape.
  bytes[4] = InitValueStore::MAX_VALUE_BYTES;
  bytes.pop_back();
  CHECK(s.decode(bytes.data(), static_cast<uint16_t>(bytes.size())));
}

// The two halves together: what the component actually does on a restart is
// load() the block and then decode() its payload, and a block the guard
// rejects must never reach the decoder at all.
void test_through_the_retention_block() {
  RetentionBlock block;
  std::memset(&block, 0xA5, sizeof(block));

  uint8_t out[RetentionBlock::CAPACITY];
  uint16_t out_len = 0;
  CHECK(!block.load(out, sizeof(out), out_len));

  const auto bytes = encoded_nine();
  CHECK(block.store(bytes.data(), static_cast<uint16_t>(bytes.size())));
  CHECK(block.load(out, sizeof(out), out_len));
  InitValueStore s;
  CHECK(s.decode(out, out_len));
  CHECK(collect(s).size() == atlantic_v5::INIT_CADENCE_COUNT);

  // A block written under a different layout version is refused by the guard,
  // so its payload is never offered to the decoder under the new meaning.
  auto *raw = reinterpret_cast<uint8_t *>(&block);
  raw[8] = static_cast<uint8_t>(raw[8] + 1);
  CHECK(!block.load(out, sizeof(out), out_len));

  // And a payload corrupted in place fails the CRC rather than decoding.
  raw[8] = static_cast<uint8_t>(raw[8] - 1);
  raw[12] = static_cast<uint8_t>(raw[12] + 1);
  CHECK(!block.load(out, sizeof(out), out_len));
}

}  // namespace

int main() {
  test_round_trip_of_the_whole_set();
  test_observe_reports_only_real_changes();
  test_same_id_different_kind_is_a_change();
  test_non_init_cadence_values_are_ignored();
  test_empty_store_encodes_nothing();
  test_partial_set_round_trips();
  test_encode_refuses_a_buffer_it_would_overrun();
  test_empty_input_is_rejected();
  test_truncated_input_is_rejected();
  test_trailing_bytes_are_rejected();
  test_record_count_above_the_set_is_rejected();
  test_foreign_entity_id_is_rejected();
  test_duplicate_entity_id_is_rejected();
  test_unknown_kind_is_rejected();
  test_value_length_must_match_the_kind();
  test_oversized_text_is_rejected();
  test_through_the_retention_block();
  TEST_MAIN_RETURN();
}
