// L1 core. Host-compilable: no esp-idf, no ESPHome, no Arduino, no exceptions, no heap.
#pragma once

#include <cstddef>
#include <cstdint>

#include "frame.h"

namespace atlantic_v5 {

// Emitted by Decoder::decode, one per catalogued value inside a frame's payload
// (plan 3.3). id is an EntityId (core/catalog.h); kept as a raw uint16_t here so
// this header doesn't need to depend on catalog.h.
struct DecodedValue {
  enum class Kind : uint8_t { FLOAT, BOOL, TEXT, UINT } kind;
  uint16_t id;
  float f = 0;
  bool b = false;
  uint32_t u = 0;
  char text[24] = {};
};

// The seven payload codecs (plan 2.6), each operating on a caller-validated
// payload pointer. Exposed individually so each is unit-testable on its own,
// independent of header dispatch.
namespace codec {

// int16 big endian, hundredths of a degree Celsius.
float decode_temp(const uint8_t *b);
// uint32 big endian.
uint32_t decode_u32(const uint8_t *b);
// uint16 big endian.
uint16_t decode_u16(const uint8_t *b);
// Single byte, 0 or 1.
bool decode_bool(const uint8_t *b);

// Copies up to len bytes until the first NUL into out (NUL-terminated, capacity
// out_len). Returns false, leaving out untouched, if the payload's last byte
// isn't 0x00 (plan 2.6: "reject if the last payload byte is not 0x00").
bool decode_text(const uint8_t *b, uint8_t len, char *out, size_t out_len);

// 5-byte `00 <min:int16> <max:int16>`. Returns false, leaving min_c/max_c
// untouched, if byte 0 isn't 0x00.
bool decode_minmax(const uint8_t *b, float *min_c, float *max_c);

// Three uint32 BE: secs_in_state0, secs_in_state1, cycle_count. active is
// secs_in_state1 > 0 (plan 2.6 cycle triplet semantics).
struct Cycle {
  bool active;
  uint32_t count;
};
Cycle decode_cycle(const uint8_t *b);

}  // namespace codec

// Header dispatch + DecodedValue emission for the message catalogue (plan 3.3).
class Decoder {
 public:
  using Sink = void (*)(void *ctx, const DecodedValue &);

  // Dispatches on f.header_key(). Never decodes a frame whose CRC failed (plan
  // 2.6). Validates payload length against the header's expected length before
  // decoding; a mismatch is counted (stats().length_mismatches) and the frame
  // is skipped, never partially decoded. A header outside the catalogue counts
  // as stats().unknown_headers and emits nothing (passthrough-and-count).
  void decode(const Frame &f, Sink sink, void *ctx) const;

  struct Stats {
    uint32_t unknown_headers = 0;
    uint32_t length_mismatches = 0;
  };
  const Stats &stats() const { return stats_; }

 private:
  bool check_length(const Frame &f, uint8_t expected) const;
  void emit_minmax(const Frame &f, Sink sink, void *ctx, uint16_t min_id, uint16_t max_id) const;
  void emit_cycle(const Frame &f, Sink sink, void *ctx, uint16_t active_id, uint16_t count_id) const;

  mutable Stats stats_;
};

}  // namespace atlantic_v5
