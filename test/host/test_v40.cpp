// Core test: the V40 / hot-water-available calculation (spec .scratch/v40-hot-water).
// Expected values are worked by hand from the spec, not recomputed with the formula.
#include <cmath>

#include "test_harness.h"
#include "v40.h"

using atlantic_v5::DEFAULT_COLD_WATER_C;
using atlantic_v5::hot_water_percent;
using atlantic_v5::v40_available;
using atlantic_v5::v40_capacity;

namespace {

bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) < eps; }

void test_available() {
  // Reference: 200 L tank, 15 C cold water. 55 C -> (55-15)/(40-15) = 1.6 -> 320 L.
  CHECK(near(v40_available(200, 15, 55), 320.0f));
  CHECK(near(v40_available(200, 15, 70), 440.0f));

  // At or below 40 C the water is not usable as 40 C water at all.
  CHECK(v40_available(200, 15, 40) == 0.0f);
  CHECK(v40_available(200, 15, 39.9f) == 0.0f);
  CHECK(v40_available(200, 15, 10) == 0.0f);

  CHECK(near(v40_available(200, 10, 50), 266.67f));
}

void test_capacity() {
  // Cloud capability 270 reports 376 L for this appliance.
  CHECK(near(v40_capacity(200, 15, 62), 376.0f));
  CHECK(near(v40_capacity(200, DEFAULT_COLD_WATER_C, 62), 376.0f));
  CHECK(near(v40_capacity(300, 15, 65), 600.0f));
}

void test_hot_water_percent() {
  CHECK(near(hot_water_percent(v40_available(200, 15, 62), v40_capacity(200, 15, 62)), 100.0f));
  CHECK(near(hot_water_percent(v40_available(200, 15, 55), v40_capacity(200, 15, 62)), 85.11f));
  // Above 100 % after an anti-legionella cycle: no upper clamp.
  CHECK(near(hot_water_percent(v40_available(200, 15, 70), v40_capacity(200, 15, 62)), 117.02f));
  // Never below 0 %, even for a cold tank.
  CHECK(hot_water_percent(v40_available(200, 15, 20), v40_capacity(200, 15, 62)) == 0.0f);
}

void test_unknown_inputs() {
  const float nan = std::nanf("");
  CHECK(std::isnan(v40_available(nan, 15, 55)));
  CHECK(std::isnan(v40_available(200, nan, 55)));
  CHECK(std::isnan(v40_available(200, 15, nan)));
  CHECK(std::isnan(v40_capacity(nan, 15, 62)));
  CHECK(std::isnan(v40_capacity(200, nan, 62)));
  CHECK(std::isnan(v40_capacity(200, 15, nan)));
  CHECK(std::isnan(hot_water_percent(nan, 376)));
  CHECK(std::isnan(hot_water_percent(320, nan)));
}

void test_degenerate_inputs() {
  // Cold water at or above 40 C makes the formula divide by zero or flip sign.
  CHECK(std::isnan(v40_available(200, 40, 55)));
  CHECK(std::isnan(v40_capacity(200, 40, 62)));
  // A setpoint_max not above cold water has no capacity to be a percentage of.
  CHECK(std::isnan(hot_water_percent(100, 0)));
  CHECK(std::isnan(hot_water_percent(100, -5)));
}

}  // namespace

int main() {
  test_available();
  test_capacity();
  test_hot_water_percent();
  test_unknown_inputs();
  test_degenerate_inputs();
  TEST_MAIN_RETURN();
}
