#include "atlantic_v5_switch.h"

#if defined(USE_SWITCH) && defined(ATLANTIC_V5_LINK_BLACKOUT)

namespace esphome {
namespace atlantic_v5_component {

// Never restored from flash: a blackout that survives a reboot is exactly the
// stranded-control-link failure the auto-release exists to prevent.
void AtlanticV5LinkBlackoutSwitch::setup() { this->publish_state(false); }

// The relay releases the blackout on its own clock, so the entity has to follow
// the relay rather than the last command it was given.
void AtlanticV5LinkBlackoutSwitch::loop() {
  bool engaged = this->parent_->link_blackout_engaged();
  if (engaged != this->state)
    this->publish_state(engaged);
}

void AtlanticV5LinkBlackoutSwitch::write_state(bool state) {
  this->parent_->request_link_blackout(state);
  this->publish_state(this->parent_->link_blackout_engaged());
}

}  // namespace atlantic_v5_component
}  // namespace esphome

#endif  // USE_SWITCH && ATLANTIC_V5_LINK_BLACKOUT
