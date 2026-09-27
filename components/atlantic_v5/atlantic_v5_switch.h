// L3 ESPHome glue. The raw_frame_dump switch: writes into the hub's
// raw_frame_dump_ flag, so it earns its own dedicated class (ADR 0001), unlike
// the read-only sensor/binary_sensor/text_sensor platforms.
#pragma once

// Included before the USE_SWITCH check below: this header is the first thing
// atlantic_v5_switch.cpp includes, so nothing has pulled defines.h in yet -
// unlike ESPHome's generated main.cpp, which reaches USE_SWITCH already
// defined via its own earlier includes.
#include "esphome/core/defines.h"

#ifdef USE_SWITCH

#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "atlantic_v5.h"

namespace esphome {
namespace atlantic_v5_component {

class AtlanticV5RawFrameDumpSwitch : public Component,
                                      public switch_::Switch,
                                      public Parented<AtlanticV5Component> {
 public:
  void setup() override;

 protected:
  void write_state(bool state) override;
};

}  // namespace atlantic_v5_component
}  // namespace esphome

#endif  // USE_SWITCH
