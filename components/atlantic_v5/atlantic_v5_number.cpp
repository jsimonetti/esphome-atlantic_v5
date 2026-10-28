#include "atlantic_v5_number.h"

#ifdef USE_NUMBER

#include <cmath>

namespace esphome {
namespace atlantic_v5_component {

// Starts at 0 (override off) after every restart; the value is never restored.
void AtlanticV5Number::setup() { this->publish_state(0.0f); }

void AtlanticV5Number::control(float value) {
  this->publish_state(value);
  // Wire unit is 0.01 degC; the number's 0..100 range keeps this inside int16_t.
  this->parent_->set_temperature_override(static_cast<int16_t>(std::lround(value * 100.0f)));
}

}  // namespace atlantic_v5_component
}  // namespace esphome

#endif  // USE_NUMBER
