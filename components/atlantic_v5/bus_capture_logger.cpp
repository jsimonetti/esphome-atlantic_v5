#include "bus_capture_logger.h"

#ifdef USE_ESP32

#include <cstring>

#include "esphome/core/log.h"

namespace esphome {
namespace atlantic_v5_component {

namespace {
constexpr int64_t CAPTURE_SILENCE_US = 4000;  // the framing backstop, reused as-is

// Uppercase hex, no separators, matching the test/captures/*.csv hex column.
void to_hex(const uint8_t *data, size_t len, char *out) {
  static const char DIGITS[] = "0123456789ABCDEF";
  for (size_t i = 0; i < len; i++) {
    out[i * 2] = DIGITS[data[i] >> 4];
    out[i * 2 + 1] = DIGITS[data[i] & 0x0F];
  }
  out[len * 2] = '\0';
}
}  // namespace

static const char *const CAPTURE_TAG = "atlantic_v5.bus_capture";

void BusCaptureLogger::flush(int64_t t_us) {
  char hex[BUF_LEN * 2 + 1];
  to_hex(this->buf_, this->len_, hex);
  ESP_LOGD(CAPTURE_TAG, "BUSCAP,%lld,%s,%s", static_cast<long long>(t_us), this->channel_tag_, hex);
  this->len_ = 0;
}

void BusCaptureLogger::feed(const uint8_t *chunk, size_t len, int64_t now_us) {
  size_t offset = 0;
  while (offset < len) {
    if (this->len_ >= sizeof(this->buf_))
      flush(now_us);
    size_t space = sizeof(this->buf_) - this->len_;
    size_t remaining = len - offset;
    size_t copy_len = remaining < space ? remaining : space;
    memcpy(this->buf_ + this->len_, chunk + offset, copy_len);
    this->len_ += copy_len;
    offset += copy_len;
  }
  if (len > 0)
    this->last_byte_us_ = now_us;

  if (this->len_ == 0)
    return;

  bool silence_elapsed = (now_us - this->last_byte_us_) >= CAPTURE_SILENCE_US;
  bool buffer_full = this->len_ >= sizeof(this->buf_);
  if (silence_elapsed || buffer_full)
    flush(this->last_byte_us_);
}

}  // namespace atlantic_v5_component
}  // namespace esphome

#endif  // USE_ESP32
