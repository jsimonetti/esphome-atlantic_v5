// Core test: the retention block's codec and its validity guard.
// Host-only - the .noinit placement itself is an ESP-IDF property and is
// probed on hardware (see .scratch/init-value-retention); what is testable
// here is that nothing but a block this firmware wrote is ever decoded.
#include <cstdint>
#include <cstring>
#include <string>

#include "retention.h"
#include "test_harness.h"

using atlantic_v5::RetentionBlock;

namespace {

// Stands in for the uninitialised SRAM the real block lives in. Filled with a
// non-zero pattern so that "all zeroes happens to be rejected" cannot pass for
// a working guard.
RetentionBlock garbage_block() {
  RetentionBlock b;
  std::memset(&b, 0xA5, sizeof(b));
  return b;
}

bool store_string(RetentionBlock &b, const std::string &s) {
  return b.store(reinterpret_cast<const uint8_t *>(s.data()), static_cast<uint16_t>(s.size()));
}

bool load_string(const RetentionBlock &b, std::string &out) {
  uint8_t buf[RetentionBlock::CAPACITY];
  uint16_t len = 0;
  if (!b.load(buf, sizeof(buf), len))
    return false;
  out.assign(reinterpret_cast<const char *>(buf), len);
  return true;
}

void test_round_trip() {
  RetentionBlock b = garbage_block();
  CHECK(store_string(b, "probe-value-1"));
  std::string out;
  CHECK(load_string(b, out));
  CHECK(out == "probe-value-1");
}

void test_empty_payload_is_a_value() {
  RetentionBlock b = garbage_block();
  CHECK(store_string(b, ""));
  std::string out = "unset";
  CHECK(load_string(b, out));
  CHECK(out.empty());
}

void test_full_capacity() {
  RetentionBlock b = garbage_block();
  const std::string full(RetentionBlock::CAPACITY, 'x');
  CHECK(store_string(b, full));
  std::string out;
  CHECK(load_string(b, out));
  CHECK(out == full);
}

// An oversized store must not leave a block a later load would accept: a
// half-written block is exactly the stale-data case the guard exists for.
void test_oversized_store_invalidates() {
  RetentionBlock b = garbage_block();
  CHECK(store_string(b, "good"));
  const std::string too_big(RetentionBlock::CAPACITY + 1, 'x');
  CHECK(!store_string(b, too_big));
  std::string out;
  CHECK(!load_string(b, out));
}

void test_uninitialised_memory_is_rejected() {
  RetentionBlock b = garbage_block();
  std::string out;
  CHECK(!load_string(b, out));

  RetentionBlock zeroed;
  std::memset(&zeroed, 0, sizeof(zeroed));
  CHECK(!load_string(zeroed, out));
}

// Byte-level tampering, applied through the raw bytes because the block's own
// fields are private. Offsets follow the documented layout:
// magic(4) crc(2) layout_version(2) payload_len(2) payload(CAPACITY).
void corrupt(RetentionBlock &b, size_t offset, uint8_t delta) {
  auto *raw = reinterpret_cast<uint8_t *>(&b);
  raw[offset] = static_cast<uint8_t>(raw[offset] + delta);
}

void test_wrong_magic_is_rejected() {
  RetentionBlock b = garbage_block();
  CHECK(store_string(b, "probe"));
  corrupt(b, 0, 1);
  std::string out;
  CHECK(!load_string(b, out));
}

void test_wrong_layout_version_is_rejected() {
  RetentionBlock b = garbage_block();
  CHECK(store_string(b, "probe"));
  corrupt(b, 8, 1);
  std::string out;
  CHECK(!load_string(b, out));
}

void test_corrupt_payload_is_rejected() {
  RetentionBlock b = garbage_block();
  CHECK(store_string(b, "probe"));
  corrupt(b, 12, 1);
  std::string out;
  CHECK(!load_string(b, out));
}

// A length that stays within capacity still has to agree with the CRC, or
// garbage trailing bytes get published as part of the value.
void test_corrupt_length_is_rejected() {  RetentionBlock b = garbage_block();
  CHECK(store_string(b, "probe"));
  corrupt(b, 10, 1);
  std::string out;
  CHECK(!load_string(b, out));
}

void test_out_of_range_length_is_rejected() {
  RetentionBlock b = garbage_block();
  CHECK(store_string(b, "probe"));
  auto *raw = reinterpret_cast<uint8_t *>(&b);
  raw[10] = 0xFF;
  raw[11] = 0xFF;
  std::string out;
  CHECK(!load_string(b, out));
}

// A caller whose buffer is smaller than the retained payload gets a refusal,
// not a silent truncation.
void test_short_output_buffer_is_refused() {
  RetentionBlock b = garbage_block();
  CHECK(store_string(b, "probe"));
  uint8_t small[2];
  uint16_t len = 0;
  CHECK(!b.load(small, sizeof(small), len));
}

}  // namespace

int main() {
  test_round_trip();
  test_empty_payload_is_a_value();
  test_full_capacity();
  test_oversized_store_invalidates();
  test_uninitialised_memory_is_rejected();
  test_wrong_magic_is_rejected();
  test_wrong_layout_version_is_rejected();
  test_corrupt_payload_is_rejected();
  test_corrupt_length_is_rejected();
  test_out_of_range_length_is_rejected();
  test_short_output_buffer_is_refused();
  TEST_MAIN_RETURN();
}
