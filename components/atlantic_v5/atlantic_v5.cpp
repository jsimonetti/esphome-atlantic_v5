#include "atlantic_v5.h"
#include "esphome/core/log.h"

#include <cstring>

#ifdef USE_ESP32
#include <esp_timer.h>
#endif

namespace esphome {
namespace atlantic_v5 {

static const char *const TAG = "atlantic_v5";
static const char *const CAPTURE_TAG = "atlantic_v5.bus_capture";

#ifdef USE_ESP32
namespace {
// Plan 2.1/2.5.1: primary framing is length-driven, this is only the backstop.
constexpr int64_t CAPTURE_SILENCE_US = 4000;

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
#endif

void AtlanticV5Component::setup() {
#ifdef USE_ESP32
  if (this->mode_ != Mode::LISTENER) {
    // MITM transport (3.5, 3.6) isn't built yet; Python-side validation
    // already rejects mode: mitm, this is a defensive backstop.
    ESP_LOGE(TAG, "mode: mitm is not implemented yet");
    this->mark_failed();
    return;
  }
  if (this->rx_pin_ < 0) {
    ESP_LOGE(TAG, "listener mode requires rx_pin");
    this->mark_failed();
    return;
  }

  this->port_ = static_cast<uart_port_t>(this->uart_num_);
  uart_config_t cfg{};
  cfg.baud_rate = 38400;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;
  uart_param_config(this->port_, &cfg);
  // TX pin is UART_PIN_NO_CHANGE: listener mode never drives the bus (G2).
  int tx_pin = this->tx_pin_ >= 0 ? this->tx_pin_ : UART_PIN_NO_CHANGE;
  uart_set_pin(this->port_, tx_pin, this->rx_pin_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  uart_driver_install(this->port_, 512, 0, 0, nullptr, 0);
  uart_set_rx_full_threshold(this->port_, 1);
  uart_set_rx_timeout(this->port_, 2);
#endif
}

void AtlanticV5Component::loop() {
#ifdef USE_ESP32
  if (this->mode_ != Mode::LISTENER || !this->bus_capture_ || this->is_failed())
    return;

  uint8_t chunk[CAPTURE_BUF_LEN];
  // Non-blocking poll: framing/decoding (M1-M3) will replace this with the
  // FrameAssembler; bus_capture only needs "whatever arrived since last loop()".
  int len = uart_read_bytes(this->port_, chunk, sizeof(chunk), 0);
  int64_t now_us = esp_timer_get_time();

  auto flush = [&](int64_t t_us) {
    // Approximates the "last byte received" convention (2.1/3.2): timestamped
    // after the read, not per-byte. Good enough for harvesting, not for timing.
    char hex[CAPTURE_BUF_LEN * 2 + 1];
    to_hex(this->capture_buf_, this->capture_len_, hex);
    ESP_LOGI(CAPTURE_TAG, "BUSCAP,%lld,bus,%s", static_cast<long long>(t_us), hex);
    this->capture_len_ = 0;
  };

  // Accumulate rather than logging immediately: frames are variable-length
  // (5-byte header + optional length-driven payload, plan 2.3), so there's no
  // fixed size to read in one shot. Only flush early (mid-chunk) if the
  // buffer would otherwise overflow, so bytes are never dropped.
  size_t offset = 0;
  while (offset < static_cast<size_t>(len)) {
    if (this->capture_len_ >= sizeof(this->capture_buf_))
      flush(now_us);
    size_t space = sizeof(this->capture_buf_) - this->capture_len_;
    size_t remaining = static_cast<size_t>(len) - offset;
    size_t copy_len = remaining < space ? remaining : space;
    memcpy(this->capture_buf_ + this->capture_len_, chunk + offset, copy_len);
    this->capture_len_ += copy_len;
    offset += copy_len;
  }
  if (len > 0)
    this->capture_last_byte_us_ = now_us;

  if (this->capture_len_ == 0)
    return;

  // Flush on the plan's 4 ms silence backstop (2.1/2.5.1) or when the buffer
  // hits the max observed frame size, whichever comes first.
  bool silence_elapsed = (now_us - this->capture_last_byte_us_) >= CAPTURE_SILENCE_US;
  bool buffer_full = this->capture_len_ >= sizeof(this->capture_buf_);
  if (silence_elapsed || buffer_full)
    flush(this->capture_last_byte_us_);
#endif
}

void AtlanticV5Component::dump_config() {
  ESP_LOGCONFIG(TAG, "Atlantic V5:");
  ESP_LOGCONFIG(TAG, "  Mode: %s", mode_ == Mode::MITM ? "mitm" : "listener");
  ESP_LOGCONFIG(TAG, "  Bus capture: %s", YESNO(bus_capture_));
}

}  // namespace atlantic_v5
}  // namespace esphome
