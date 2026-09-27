#include "atlantic_v5_select.h"

#ifdef USE_SELECT

#include "types.h"

namespace esphome {
namespace atlantic_v5_component {

// Options are declared in select.py in ControlMode enum order (PASSTHROUGH=0
// .. BOOST=4, plan 2.8's table), so the index IS the ControlMode value - no
// string lookup needed on the control path.
void AtlanticV5Select::setup() { this->publish_state(this->option_at(0)); }

void AtlanticV5Select::control(size_t index) {
  this->publish_state(index);
  this->parent_->set_control_mode(static_cast<::atlantic_v5::ControlMode>(index));
}

}  // namespace atlantic_v5_component
}  // namespace esphome

#endif  // USE_SELECT
