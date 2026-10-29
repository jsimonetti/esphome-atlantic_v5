// L1 core. Host-compilable: no esp-idf, no ESPHome. The temperature publish
// step (CONTEXT.md): temperatures reach entities in fixed 0.1 C steps.
#pragma once

#include <cmath>

namespace atlantic_v5 {

constexpr float TEMP_PUBLISH_STEP_C = 0.1f;

// The bus resolves 0.01 C, so compare in whole hundredths: a float difference
// such as 23.1f - 23.0f lands a hair under 0.1 and would never trip a >= test.
inline long temp_hundredths(float c) {
  return std::lround(c * 100.0f);
}

inline float quantise_temp(float c) {
  return static_cast<float>(std::lround(c * 10.0f)) / 10.0f;
}

// last is the previously published (quantised) value.
inline bool temp_should_publish(bool has_last, float last, float raw) {
  if (!has_last)
    return true;
  const long delta = temp_hundredths(raw) - temp_hundredths(last);
  return delta >= 10 || delta <= -10;
}

}  // namespace atlantic_v5
