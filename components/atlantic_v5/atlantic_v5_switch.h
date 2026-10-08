// L3 ESPHome glue. The link_blackout switch: like the control_mode select it
// writes into shared runtime state the relay task reads, so it earns its own
// class (ADR 0001). Unlike the select it is compile-time opt-in — the whole
// file, and the Relay drop path it drives, only exist when switch.py put the
// entity in the config.
#pragma once

// Included before the USE_SWITCH check below for the same reason as in
// atlantic_v5_select.h: this header is the first thing atlantic_v5_switch.cpp
// includes, so nothing has pulled defines.h in yet.
#include "esphome/core/defines.h"

#if defined(USE_SWITCH) && defined(ATLANTIC_V5_LINK_BLACKOUT)

#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "atlantic_v5.h"

namespace esphome {
namespace atlantic_v5_component {

class AtlanticV5LinkBlackoutSwitch : public Component, public switch_::Switch, public Parented<AtlanticV5Component> {
 public:
  void setup() override;
  void loop() override;

 protected:
  void write_state(bool state) override;
};

}  // namespace atlantic_v5_component
}  // namespace esphome

#endif  // USE_SWITCH && ATLANTIC_V5_LINK_BLACKOUT
