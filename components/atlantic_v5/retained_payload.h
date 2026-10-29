// L1 core. Host-compilable: no esp-idf, no ESPHome, no Arduino, no exceptions, no heap.
#pragma once

#include <cstdint>

#include "init_values.h"
#include "retention.h"

namespace atlantic_v5 {

// What a RetentionBlock carries across a software restart: two independent
// sections, the init-cadence values (init_values.h) and the cold water
// temperature, which is a user-supplied parameter and not an observation
// (ADR 0003).
//
// Layout: flags(1), then if bit 0 an init section u16 length + InitValueStore
// encoding, then if bit 1 the cold water temperature as a native-endian float
// (the block never leaves the device). Any other bit, a short or long input, or a cold value outside
// the number's range rejects the whole payload.
class RetainedPayload {
 public:
  static constexpr uint16_t MAX_ENCODED = 1 + 2 + InitValueStore::MAX_ENCODED + 4;

  // Returns the bytes written, or 0 when there is nothing to retain or it does
  // not fit. cold may be null.
  static uint16_t encode(const InitValueStore &init, const float *cold, uint8_t *out, uint16_t cap);

  // All-or-nothing: on false, init, has_cold and cold are left unchanged.
  static bool decode(const uint8_t *data, uint16_t len, InitValueStore &init, bool &has_cold, float &cold);
};

static_assert(RetainedPayload::MAX_ENCODED <= RetentionBlock::CAPACITY,
              "the retention block must hold every init-cadence value and the cold water temperature");

}  // namespace atlantic_v5
