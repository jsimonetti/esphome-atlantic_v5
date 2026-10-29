// Core test: the temperature publish step (CONTEXT.md) and the catalogue's
// is_temperature(). Expected values are worked by hand, not recomputed.
#include <cstring>
#include <string>

#include "catalog.h"
#include "temp_step.h"
#include "test_harness.h"

using namespace atlantic_v5;

namespace {

void test_quantise_rounds_to_tenth() {
  CHECK(quantise_temp(23.04f) == 23.0f);
  CHECK(quantise_temp(23.06f) == 23.1f);
  CHECK(quantise_temp(23.14f) == 23.1f);
  CHECK(quantise_temp(23.16f) == 23.2f);
  CHECK(quantise_temp(-0.04f) == 0.0f);
  CHECK(quantise_temp(-5.26f) == -5.3f);
}

void test_first_value_always_publishes() {
  CHECK(temp_should_publish(false, 0.0f, 23.04f));
}

void test_movement_below_a_step_is_quiet() {
  CHECK(!temp_should_publish(true, 23.0f, 23.0f));
  CHECK(!temp_should_publish(true, 23.0f, 23.06f));
  CHECK(!temp_should_publish(true, 23.0f, 23.09f));
  CHECK(!temp_should_publish(true, 23.0f, 22.91f));
}

void test_a_full_step_publishes_in_both_directions() {
  CHECK(temp_should_publish(true, 23.0f, 23.10f));
  CHECK(temp_should_publish(true, 23.0f, 22.90f));
  CHECK(temp_should_publish(true, 23.0f, 25.0f));
}

// Plain rounding would toggle 23.0/23.1 for a sensor wobbling around x.x5.
void test_wobble_around_a_rounding_boundary_does_not_flap() {
  const float wobble[] = {23.04f, 23.06f, 23.04f, 23.06f, 23.05f};
  float last = quantise_temp(wobble[0]);
  for (float raw : wobble)
    CHECK(!temp_should_publish(true, last, raw));
  // ...and once it genuinely moves, it publishes once and then holds.
  CHECK(temp_should_publish(true, last, 23.10f));
  last = quantise_temp(23.10f);
  CHECK(!temp_should_publish(true, last, 23.05f));
  CHECK(!temp_should_publish(true, last, 23.14f));
}

void test_is_temperature_matches_entity_names() {
  int temperatures = 0;
  for (uint16_t id = 0; id < ENT_COUNT; id++) {
    const std::string name = entity_name(static_cast<EntityId>(id));
    const bool expected = name.rfind("setpoint_", 0) == 0 || name.find("_temperature") != std::string::npos;
    CHECK(is_temperature(id) == expected);
    temperatures += is_temperature(id);
  }
  CHECK(temperatures == 20);
  CHECK(!is_temperature(ENT_COUNT));
}

}  // namespace

int main() {
  test_quantise_rounds_to_tenth();
  test_first_value_always_publishes();
  test_movement_below_a_step_is_quiet();
  test_a_full_step_publishes_in_both_directions();
  test_wobble_around_a_rounding_boundary_does_not_flap();
  test_is_temperature_matches_entity_names();
  TEST_MAIN_RETURN();
}
