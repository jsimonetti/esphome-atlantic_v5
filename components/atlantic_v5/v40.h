// L1 core. Host-compilable: no esp-idf, no ESPHome, no Arduino, no exceptions, no heap.
// V40 and hot water available (CONTEXT.md; README "V40 and hot water available").
// The tank is treated as uniformly at the measured temperature. Unknown inputs
// are NaN and give NaN.
#pragma once

namespace atlantic_v5 {

constexpr float DEFAULT_COLD_WATER_C = 15.0f;
constexpr float COLD_WATER_MIN_C = 0.0f;
constexpr float COLD_WATER_MAX_C = 30.0f;

// Litres of 40 C water the tank holds: 0 at or below 40 C, else
// tank_l * (water_c - cold_c) / (40 - cold_c). NaN if cold_c >= 40.
float v40_available(float tank_l, float cold_c, float water_c);

// V40 with the tank at setpoint_max: tank_l * (setmax_c - cold_c) / (40 - cold_c).
float v40_capacity(float tank_l, float cold_c, float setmax_c);

// 100 * available / capacity. Not clamped above (an anti-legionella cycle
// exceeds 100 %); NaN when capacity is not positive.
float hot_water_percent(float available_l, float capacity_l);

}  // namespace atlantic_v5
