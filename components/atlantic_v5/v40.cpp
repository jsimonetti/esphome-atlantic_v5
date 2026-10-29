#include "v40.h"

#include <cmath>

namespace atlantic_v5 {

namespace {
constexpr float V40_REFERENCE_C = 40.0f;

bool usable_cold(float cold_c) { return !std::isnan(cold_c) && cold_c < V40_REFERENCE_C; }
}  // namespace

float v40_available(float tank_l, float cold_c, float water_c) {
  if (std::isnan(tank_l) || std::isnan(water_c) || !usable_cold(cold_c))
    return NAN;
  if (water_c <= V40_REFERENCE_C)
    return 0.0f;
  return tank_l * (water_c - cold_c) / (V40_REFERENCE_C - cold_c);
}

float v40_capacity(float tank_l, float cold_c, float setmax_c) {
  if (std::isnan(tank_l) || std::isnan(setmax_c) || !usable_cold(cold_c))
    return NAN;
  return tank_l * (setmax_c - cold_c) / (V40_REFERENCE_C - cold_c);
}

float hot_water_percent(float available_l, float capacity_l) {
  if (std::isnan(available_l) || std::isnan(capacity_l) || capacity_l <= 0.0f)
    return NAN;
  return 100.0f * available_l / capacity_l;
}

}  // namespace atlantic_v5
