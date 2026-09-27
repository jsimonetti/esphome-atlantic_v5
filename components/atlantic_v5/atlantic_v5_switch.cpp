#include "atlantic_v5_switch.h"

#ifdef USE_SWITCH

namespace esphome {
namespace atlantic_v5_component {

// Off by default, like every other diagnostic; switch_::Switch's own
// RESTORE_MODE_ALWAYS_OFF default (switch.py) means no explicit state to
// publish here beyond keeping the hub's flag in sync at boot.
void AtlanticV5RawFrameDumpSwitch::setup() {
  this->publish_state(false);
  this->parent_->set_raw_frame_dump(false);
}

void AtlanticV5RawFrameDumpSwitch::write_state(bool state) {
  this->publish_state(state);
  this->parent_->set_raw_frame_dump(state);
}

}  // namespace atlantic_v5_component
}  // namespace esphome

#endif  // USE_SWITCH
