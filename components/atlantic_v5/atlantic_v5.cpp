#include "atlantic_v5.h"
#include "esphome/core/log.h"

namespace esphome {
namespace atlantic_v5 {

static const char *const TAG = "atlantic_v5";

void AtlanticV5Component::setup() {
  // M0 skeleton: no UART bring-up, no entities yet.
}

void AtlanticV5Component::loop() {
  // M0 skeleton: nothing to drain yet.
}

void AtlanticV5Component::dump_config() {
  ESP_LOGCONFIG(TAG, "Atlantic V5:");
  ESP_LOGCONFIG(TAG, "  Mode: %s", mode_ == Mode::MITM ? "mitm" : "listener");
}

}  // namespace atlantic_v5
}  // namespace esphome
