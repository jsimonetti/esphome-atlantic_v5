// L1 core. Host-compilable: no esp-idf, no ESPHome, no Arduino, no exceptions, no heap.
#pragma once

#include <cstddef>
#include <cstdint>

#include "catalog.h"
#include "decoder.h"
#include "retention.h"

namespace atlantic_v5 {

// The init-cadence values (CONTEXT.md) as last observed, plus the codec that
// turns them into the payload a RetentionBlock carries across a software
// restart (ADR 0003).
//
// Everything a held value means is a cache of a past observation, never a
// reading: observe() lets a live decode overwrite what was restored, and tells
// the caller when that actually changed something.
class InitValueStore {
 public:
  enum class Observation : uint8_t {
    NOT_INIT_CADENCE,  // some other entity; this store has no opinion on it
    UNCHANGED,         // already held, so the entity already carries this value
    CHANGED,           // now held; worth publishing and worth retaining
  };

  // Records v when it is an init-cadence value this store does not already
  // hold.
  Observation observe(const DecodedValue &v);

  // Serialises every held value into out[0..cap). Returns the number of bytes
  // written, or 0 when nothing is held or the encoding would not fit.
  uint16_t encode(uint8_t *out, uint16_t cap) const;

  // Replaces the held values with the ones encoded in data[0..len). Returns
  // false - holding nothing - unless the bytes are a well-formed encoding of a
  // duplicate-free set of init-cadence values.
  bool decode(const uint8_t *data, uint16_t len);

  // Emits every held value, in INIT_CADENCE order.
  void each(Decoder::Sink sink, void *ctx) const;

  static constexpr uint16_t COUNT_BYTES = 1;
  static constexpr uint16_t RECORD_HEADER_BYTES = 4;  // id (2), kind, length
  static constexpr uint16_t MAX_VALUE_BYTES = sizeof(DecodedValue::text) - 1;
  static constexpr uint16_t MAX_ENCODED =
      COUNT_BYTES + INIT_CADENCE_COUNT * (RECORD_HEADER_BYTES + MAX_VALUE_BYTES);

 private:
  // decode() minus the all-or-nothing guarantee: may leave records from a
  // prefix of the input held when it fails partway through.
  bool decode_records(const uint8_t *data, uint16_t len);
  void clear();

  DecodedValue values_[INIT_CADENCE_COUNT];
  bool present_[INIT_CADENCE_COUNT]{};
};

static_assert(InitValueStore::MAX_ENCODED <= RetentionBlock::CAPACITY,
              "the retention block must be able to hold every init-cadence value at its widest");

}  // namespace atlantic_v5
