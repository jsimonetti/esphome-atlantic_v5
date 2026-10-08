// L3 glue, esp-idf only: the single RetentionBlock instance, placed in a
// linker section the C runtime startup deliberately does not clear. Never part
// of the host CMake build (see test/host/CMakeLists.txt) - the codec it holds
// is tested there, the placement can only be verified on hardware.
#pragma once

#ifdef USE_ESP32

#include "retention.h"

namespace atlantic_v5 {

// The block that survives a software restart. ESP-IDF documents __NOINIT_ATTR
// data as keeping its value across one; it documents nothing about a power
// cycle, because that is a property of volatile SRAM rather than an API
// contract. Both halves are measured, not assumed - see
// .scratch/init-value-retention.
RetentionBlock &retained_block();

}  // namespace atlantic_v5

#endif  // USE_ESP32
