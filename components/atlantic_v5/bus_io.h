// L2 transport. Abstract IO boundary: Relay only ever sees this
// interface, never a concrete UART or mock directly, which is what makes the relay
// logic verifiable on a host with no hardware and no ESPHome headers.
#pragma once

#include <cstddef>
#include <cstdint>

namespace atlantic_v5 {

class BusIo {
 public:
  // Non-blocking or timeout-bounded read of whatever bytes are currently
  // available, up to max. Returns the number of bytes copied into dst.
  virtual int read(uint8_t *dst, size_t max, uint32_t timeout_us) = 0;
  // Blocks until len bytes have been shifted out.
  virtual void write(const uint8_t *src, size_t len) = 0;
  // Discards any bytes currently buffered for read (echo suppression, 3.5.3).
  virtual void flush_input() = 0;
  virtual uint32_t now_us() = 0;
  virtual ~BusIo() = default;
};

}  // namespace atlantic_v5
