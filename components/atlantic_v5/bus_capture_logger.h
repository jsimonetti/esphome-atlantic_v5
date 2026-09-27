// L2 transport, esp-idf only. Raw pre-assembly byte+timestamp capture logging
// (plan 3.5.5), independent of FrameAssembler/Decoder/RelayPolicy. Extracted from
// ticket 07's listener-mode-only implementation so ticket 08's MITM piggyback
// (one instance per side, tagged "hmi"/"main") can share the same accumulate-on-
// silence-backstop logic instead of reimplementing it, per channel tag ("bus" for
// listener mode's single tapped wire).
#pragma once

#ifdef USE_ESP32

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace atlantic_v5_component {

class BusCaptureLogger {
 public:
  explicit BusCaptureLogger(const char *channel_tag) : channel_tag_(channel_tag) {}

  // Accumulates chunk into the current candidate frame and flushes (logs) it once
  // the plan's 4ms silence backstop (2.1/2.5.1) elapses or the buffer would
  // otherwise overflow. Frames are variable-length (plan 2.3), so there's no fixed
  // size to log in one shot; call every time bytes are read, even len == 0, so the
  // silence backstop still fires with no new bytes.
  void feed(const uint8_t *chunk, size_t len, int64_t now_us);

 private:
  static constexpr size_t BUF_LEN = 32;  // matches the documented max frame size

  void flush(int64_t t_us);

  const char *channel_tag_;
  uint8_t buf_[BUF_LEN]{};
  size_t len_{0};
  int64_t last_byte_us_{0};
};

}  // namespace atlantic_v5_component
}  // namespace esphome

#endif  // USE_ESP32
