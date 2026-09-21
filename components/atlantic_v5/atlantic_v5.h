#pragma once

#include "esphome/core/component.h"

namespace esphome {
namespace atlantic_v5 {

// Fixed at YAML/compile time (see implementation plan, non-goals: no automatic
// mode detection).
enum class Mode : uint8_t { LISTENER, MITM };

class AtlanticV5Component : public Component {
 public:
  void set_mode(Mode mode) { mode_ = mode; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  Mode mode_{Mode::LISTENER};
};

}  // namespace atlantic_v5
}  // namespace esphome
