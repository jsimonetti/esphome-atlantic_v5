// L1 core. Host-compilable: no esp-idf, no ESPHome, no Arduino, no exceptions, no heap.
#pragma once

#include <cstddef>
#include <cstdint>

namespace atlantic_v5 {

// CRC-16/MODBUS: poly 0x8005 reflected (0xA001), init 0xFFFF, reflect in/out, no final xor.
// Computed over data[0..len), per docs/protocol.md "CRC". No table, no library dependency.
uint16_t crc16_modbus(const uint8_t *data, size_t len);

}  // namespace atlantic_v5
