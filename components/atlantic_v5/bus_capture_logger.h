// L2 transport, esp-idf only. Raw pre-assembly byte+timestamp capture logging,
// independent of FrameAssembler/Decoder/RelayPolicy. Shared by both modes: MITM
// runs one instance per side (tagged "hmi"/"main"), listener one for its single
// tapped wire (tagged "bus"), so the accumulate-on-silence-backstop logic lives
// in one place.
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
  // the 4ms silence backstop elapses or the buffer would
  // otherwise overflow. Frames are variable-length, so there's no fixed
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
