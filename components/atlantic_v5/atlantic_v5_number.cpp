#include "atlantic_v5_number.h"

#ifdef USE_NUMBER

namespace esphome {
namespace atlantic_v5_component {

void AtlanticV5Number::setup() { this->publish_state(this->parent_->cold_water_temperature()); }

void AtlanticV5Number::control(float value) {
  this->parent_->set_cold_water_temperature(value);
  this->publish_state(value);
}

}  // namespace atlantic_v5_component
}  // namespace esphome

#endif  // USE_NUMBER
