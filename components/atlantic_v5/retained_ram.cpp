#include "retained_ram.h"

#ifdef USE_ESP32

#include <esp_attr.h>

namespace atlantic_v5 {

namespace {
// __NOINIT_ATTR, not a plain global: a plain global lands in .bss and is zeroed
// on every startup, which would erase the value before anything could read it.
__NOINIT_ATTR RetentionBlock g_block;
}  // namespace

RetentionBlock &retained_block() { return g_block; }

}  // namespace atlantic_v5

#endif  // USE_ESP32
