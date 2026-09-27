#include "atlantic_v5.h"
#include "esphome/core/log.h"

#include <cinttypes>
#include <cmath>
#include <cstring>

#ifdef USE_ESP32
#include <esp_timer.h>
#endif

namespace esphome {
namespace atlantic_v5_component {

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

  this->listener_.set_sink(&AtlanticV5Component::publish_trampoline, this);
  for (float &f : this->last_published_)
    f = NAN;
#endif
}

void AtlanticV5Component::loop() {
#ifdef USE_ESP32
  if (this->mode_ != Mode::LISTENER || this->is_failed())
    return;

  uint8_t chunk[CAPTURE_BUF_LEN];
  int len = uart_read_bytes(this->port_, chunk, sizeof(chunk), 0);
  int64_t now_us = esp_timer_get_time();
  // Approximates the "last byte received" convention (2.1/3.2): one timestamp
  // for the whole chunk, not per-byte. Good enough at this poll cadence, not
  // for the sub-millisecond timing MITM will need once the relay task exists.
  uint32_t now_us32 = static_cast<uint32_t>(now_us);

  if (this->bus_capture_)
    this->capture_bytes(chunk, static_cast<size_t>(len), now_us);

  for (int i = 0; i < len; i++)
    this->listener_.push_byte(chunk[i], now_us32);
  this->listener_.tick(now_us32);
  this->update_staleness(now_us32);
#endif
}

#ifdef USE_ESP32
void AtlanticV5Component::capture_bytes(const uint8_t *chunk, size_t len, int64_t now_us) {
  auto flush = [&](int64_t t_us) {
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
  while (offset < len) {
    if (this->capture_len_ >= sizeof(this->capture_buf_))
      flush(now_us);
    size_t space = sizeof(this->capture_buf_) - this->capture_len_;
    size_t remaining = len - offset;
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
}
#endif

void AtlanticV5Component::set_entity(uint16_t id, void *obj, EntityKind kind) {
  if (id >= ::atlantic_v5::ENT_COUNT)
    return;
  this->entities_[id] = obj;
  this->kinds_[id] = kind;
}

void AtlanticV5Component::publish_trampoline(void *ctx, const ::atlantic_v5::DecodedValue &v) {
  static_cast<AtlanticV5Component *>(ctx)->publish(v);
}

void AtlanticV5Component::publish(const ::atlantic_v5::DecodedValue &v) {
  if (v.id >= ::atlantic_v5::ENT_COUNT)
    return;
  void *obj = this->entities_[v.id];
  if (obj == nullptr)
    return;

  switch (this->kinds_[v.id]) {
    case EntityKind::SENSOR: {
      float f;
      if (v.kind == ::atlantic_v5::DecodedValue::Kind::FLOAT) {
        f = v.f;
      } else if (v.kind == ::atlantic_v5::DecodedValue::Kind::UINT) {
        f = static_cast<float>(v.u);
      } else {
        return;  // BOOL/TEXT never target a numeric sensor
      }
      auto *s = static_cast<sensor::Sensor *>(obj);
      // sensor::Sensor::publish_state() doesn't dedupe by value on its own
      // (unlike binary_sensor/text_sensor); do it here so the API stays quiet
      // at the bus's ~1 Hz frame rate, unless force_update overrides it.
      bool unchanged = this->has_last_published_[v.id] && this->last_published_[v.id] == f;
      if (unchanged && !s->get_force_update())
        return;
      s->publish_state(f);
      this->last_published_[v.id] = f;
      this->has_last_published_[v.id] = true;
      break;
    }
    case EntityKind::BINARY_SENSOR: {
      if (v.kind != ::atlantic_v5::DecodedValue::Kind::BOOL)
        return;
      static_cast<binary_sensor::BinarySensor *>(obj)->publish_state(v.b);
      break;
    }
    case EntityKind::TEXT_SENSOR: {
      if (v.kind != ::atlantic_v5::DecodedValue::Kind::TEXT)
        return;
      static_cast<text_sensor::TextSensor *>(obj)->publish_state(v.text);
      break;
    }
  }
}

void AtlanticV5Component::update_staleness(uint32_t now_us) {
  bool is_stale = this->listener_.us_since_main(now_us) >= this->timeout_us_;
  if (is_stale == this->stale_)
    return;
  this->stale_ = is_stale;

  if (!is_stale) {
    this->status_clear_warning();
    return;
  }

  this->status_set_warning("no data from MAIN");
  for (uint16_t id = 0; id < ::atlantic_v5::ENT_COUNT; id++) {
    if (this->entities_[id] == nullptr || this->kinds_[id] != EntityKind::SENSOR)
      continue;
    static_cast<sensor::Sensor *>(this->entities_[id])->publish_state(NAN);
    this->has_last_published_[id] = false;
  }
}

void AtlanticV5Component::dump_config() {
  ESP_LOGCONFIG(TAG, "Atlantic V5:");
  ESP_LOGCONFIG(TAG, "  Mode: %s", mode_ == Mode::MITM ? "mitm" : "listener");
  ESP_LOGCONFIG(TAG, "  Bus capture: %s", YESNO(bus_capture_));
  ESP_LOGCONFIG(TAG, "  Timeout: %" PRIu32 " ms", this->timeout_us_ / 1000);
}

}  // namespace atlantic_v5_component
}  // namespace esphome
