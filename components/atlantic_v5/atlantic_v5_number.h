// L3 ESPHome glue. The water_temperature_override number (docs/protocol.md
// "Control surface") writes into shared runtime state (RelayPolicy), so like
// the control_mode select it gets its own class (ADR 0001).
#pragma once

// Must precede the USE_NUMBER check: nothing has pulled defines.h in yet when
// atlantic_v5_number.cpp includes this header first.
#include "esphome/core/defines.h"

#ifdef USE_NUMBER

#include "esphome/components/number/number.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "atlantic_v5.h"

namespace esphome {
namespace atlantic_v5_component {

class AtlanticV5Number : public Component, public number::Number, public Parented<AtlanticV5Component> {
 public:
  void setup() override;

 protected:
  void control(float value) override;
};

}  // namespace atlantic_v5_component
}  // namespace esphome

#endif  // USE_NUMBER
