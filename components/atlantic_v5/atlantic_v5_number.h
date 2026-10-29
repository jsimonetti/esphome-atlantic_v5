// L3 ESPHome glue. The cold_water_temperature number: a User-supplied parameter
// (CONTEXT.md) that feeds the hub's V40 calculation only. Its own class per ADR 0001.
#pragma once

// Must precede the USE_NUMBER check: nothing upstream has pulled defines.h in
// when atlantic_v5_number.cpp includes this first.
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
  // After the hub, which restores the retained value in its own setup().
  float get_setup_priority() const override { return setup_priority::LATE - 1.0f; }

 protected:
  void control(float value) override;
};

}  // namespace atlantic_v5_component
}  // namespace esphome

#endif  // USE_NUMBER
