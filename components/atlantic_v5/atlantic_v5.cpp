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

namespace {
// Uppercase hex, no separators - matches bus_capture_logger.cpp's own copy;
// kept separate rather than shared since the two live in different namespaces
// (L3 esphome::atlantic_v5_component here vs. that file's ESP32-only logging).
void to_hex(const uint8_t *data, size_t len, char *out) {
  static const char DIGITS[] = "0123456789ABCDEF";
  for (size_t i = 0; i < len; i++) {
    out[i * 2] = DIGITS[data[i] >> 4];
    out[i * 2 + 1] = DIGITS[data[i] & 0x0F];
  }
  out[len * 2] = '\0';
}

// The 5-byte header key as 10 uppercase hex chars, matching entity/log
// conventions elsewhere (plan 2.3's big-endian key).
void format_header_hex(uint64_t key, char *out) {
  uint8_t bytes[::atlantic_v5::HEADER_LEN];
  for (size_t i = 0; i < ::atlantic_v5::HEADER_LEN; i++)
    bytes[i] = static_cast<uint8_t>(key >> ((::atlantic_v5::HEADER_LEN - 1 - i) * 8));
  to_hex(bytes, sizeof(bytes), out);
}
}  // namespace

void AtlanticV5Component::setup() {
#ifdef USE_ESP32
  for (float &f : this->last_published_)
    f = NAN;

  if (this->mode_ == Mode::MITM) {
    this->setup_mitm();
    return;
  }
  this->setup_listener();
#endif
}

void AtlanticV5Component::loop() {
#ifdef USE_ESP32
  if (this->is_failed())
    return;

  if (this->mode_ == Mode::MITM) {
    this->loop_mitm();
    return;
  }
  this->loop_listener();
#endif
}

#ifdef USE_ESP32
void AtlanticV5Component::setup_listener() {
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
  this->listener_.set_frame_sink(&AtlanticV5Component::frame_dump_trampoline, this);
}

void AtlanticV5Component::loop_listener() {
  uint8_t chunk[32];
  int len = uart_read_bytes(this->port_, chunk, sizeof(chunk), 0);
  int64_t now_us = esp_timer_get_time();
  // Approximates the "last byte received" convention (2.1/3.2): one timestamp
  // for the whole chunk, not per-byte. Good enough at this poll cadence, not
  // for the sub-millisecond timing MITM needs (handled by RelayTask instead).
  uint32_t now_us32 = static_cast<uint32_t>(now_us);

  if (this->bus_capture_)
    this->capture_logger_.feed(chunk, static_cast<size_t>(len), now_us);

  for (int i = 0; i < len; i++)
    this->listener_.push_byte(chunk[i], now_us32);
  this->listener_.tick(now_us32);
  this->update_staleness(now_us32);
  this->update_diagnostics(now_us32);
}

void AtlanticV5Component::set_hmi_uart(int uart_num, int rx_pin, int tx_pin, int tx_enable_pin, bool one_wire_mirror) {
  this->hmi_cfg_ = {uart_num, rx_pin, tx_pin, tx_enable_pin, one_wire_mirror};
}

void AtlanticV5Component::set_main_uart(int uart_num, int rx_pin, int tx_pin, int tx_enable_pin,
                                         bool one_wire_mirror) {
  this->main_cfg_ = {uart_num, rx_pin, tx_pin, tx_enable_pin, one_wire_mirror};
}

void AtlanticV5Component::capture_sink_trampoline(void *ctx, ::atlantic_v5::Channel channel, const uint8_t *data,
                                                   size_t len, uint32_t t_us) {
  auto *self = static_cast<AtlanticV5Component *>(ctx);
  BusCaptureLogger *logger = channel == ::atlantic_v5::Channel::HMI ? self->hmi_capture_logger_
                                                                     : self->main_capture_logger_;
  logger->feed(data, len, static_cast<int64_t>(t_us));
}

void AtlanticV5Component::setup_mitm() {
  ::atlantic_v5::UartBusIoConfig hmi_io_cfg{};
  hmi_io_cfg.port = static_cast<uart_port_t>(this->hmi_cfg_.uart_num);
  hmi_io_cfg.rx_pin = this->hmi_cfg_.rx_pin;
  hmi_io_cfg.tx_pin = this->hmi_cfg_.tx_pin;
  hmi_io_cfg.tx_enable_pin = this->hmi_cfg_.tx_enable_pin;
  hmi_io_cfg.one_wire_mirror = this->hmi_cfg_.one_wire_mirror;

  ::atlantic_v5::UartBusIoConfig main_io_cfg{};
  main_io_cfg.port = static_cast<uart_port_t>(this->main_cfg_.uart_num);
  main_io_cfg.rx_pin = this->main_cfg_.rx_pin;
  main_io_cfg.tx_pin = this->main_cfg_.tx_pin;
  main_io_cfg.tx_enable_pin = this->main_cfg_.tx_enable_pin;
  main_io_cfg.one_wire_mirror = this->main_cfg_.one_wire_mirror;

  ::atlantic_v5::RelayTask::Config cfg{};
  cfg.hmi = hmi_io_cfg;
  cfg.main = main_io_cfg;
  cfg.relay_core = this->relay_core_;
  cfg.self_test = this->self_test_;

  this->relay_task_ = new ::atlantic_v5::RelayTask(cfg, this->policy_);
  if (this->bus_capture_) {
    this->hmi_capture_logger_ = new BusCaptureLogger("hmi");
    this->main_capture_logger_ = new BusCaptureLogger("main");
    this->relay_task_->set_capture_sink(&AtlanticV5Component::capture_sink_trampoline, this);
  }
  this->relay_task_->begin();

  // plan 3.5.4 #3: "Publish the result as a diagnostic text sensor ... Never
  // block the relay on the self-test outcome" - begin() already ran it
  // synchronously before spawning the relay task, so the result is ready now.
  void *self_test_obj = this->entities_[::atlantic_v5::ENT_SELF_TEST_RESULT];
  if (self_test_obj != nullptr && this->kinds_[::atlantic_v5::ENT_SELF_TEST_RESULT] == EntityKind::TEXT_SENSOR)
    static_cast<text_sensor::TextSensor *>(self_test_obj)->publish_state(this->relay_task_->self_test_result());
}

void AtlanticV5Component::loop_mitm() {
  ::atlantic_v5::FrameEvent events[8];  // plan 3.6.5: N=8, bounds loop time
  size_t n = this->relay_task_->drain_events(events, 8);
  for (size_t i = 0; i < n; i++) {
    const auto &ev = events[i];
    if (!ev.crc_ok)
      continue;  // still forwarded on the wire (fail-safe); just not decoded
    ::atlantic_v5::Frame f(ev.channel, ev.data, ev.len);
    // Transaction-byte rule (plan 2.2): only MAIN's payload-bearing response
    // resets the staleness gate, matching ticket 07's listener-mode semantics.
    if (ev.channel == ::atlantic_v5::Channel::MAIN && f.has_payload())
      this->last_main_us_ = ev.t_us;
    this->maybe_dump_frame(f, ev.t_us);
    this->decoder_.decode(f, &AtlanticV5Component::publish_trampoline, this);
  }
  uint32_t now_us32 = static_cast<uint32_t>(esp_timer_get_time());
  this->update_staleness(now_us32);
  this->update_diagnostics(now_us32);
}

void AtlanticV5Component::set_control_mode(::atlantic_v5::ControlMode mode) { this->policy_.set_control_mode(mode); }

void AtlanticV5Component::publish_diag_uint(uint16_t id, uint32_t value) {
  ::atlantic_v5::DecodedValue v{};
  v.kind = ::atlantic_v5::DecodedValue::Kind::UINT;
  v.id = id;
  v.u = value;
  this->publish(v);
}

void AtlanticV5Component::publish_diag_float(uint16_t id, float value) {
  ::atlantic_v5::DecodedValue v{};
  v.kind = ::atlantic_v5::DecodedValue::Kind::FLOAT;
  v.id = id;
  v.f = value;
  this->publish(v);
}

void AtlanticV5Component::update_diagnostics(uint32_t now_us) {
  if (now_us - this->last_diag_us_ < 1'000'000)  // plan 3.8: no cadence specified, 1s matches the bus's own rate
    return;
  this->last_diag_us_ = now_us;

  uint32_t frames_ok = 0, crc_errors = 0, resyncs = 0, dropped_bytes = 0;
  uint32_t frames_relayed = 0, rewrites_applied = 0, echo_bytes = 0, queue_overflows = 0;
  uint32_t latency_max_us = 0, task_stack_free = 0;
  float latency_avg_us = 0;
  uint32_t unknown_frames;
  uint64_t last_unknown_header;

  if (this->mode_ == Mode::MITM) {
    const auto &hmi = this->relay_task_->hmi_stats();
    const auto &main = this->relay_task_->main_stats();
    frames_ok = hmi.frames + main.frames;
    crc_errors = hmi.crc_errors + main.crc_errors;
    resyncs = hmi.resyncs + main.resyncs;
    dropped_bytes = hmi.dropped_bytes + main.dropped_bytes;
    unknown_frames = this->decoder_.stats().unknown_headers;
    last_unknown_header = this->decoder_.stats().last_unknown_header;

    const auto &relay_stats = this->relay_task_->stats();
    frames_relayed = relay_stats.frames_relayed;
    rewrites_applied = relay_stats.rewrites_applied;
    echo_bytes = relay_stats.echo_bytes;
    latency_max_us = relay_stats.latency_max_us;
    if (relay_stats.latency_samples > 0)
      latency_avg_us = static_cast<float>(relay_stats.latency_total_us) / static_cast<float>(relay_stats.latency_samples);
    queue_overflows = this->relay_task_->queue_overflows();
    task_stack_free = this->relay_task_->stack_high_water_mark();
  } else {
    const auto &asm_stats = this->listener_.assembler_stats();
    frames_ok = asm_stats.frames;
    crc_errors = asm_stats.crc_errors;
    resyncs = asm_stats.resyncs;
    dropped_bytes = asm_stats.dropped_bytes;
    unknown_frames = this->listener_.decoder_stats().unknown_headers;
    last_unknown_header = this->listener_.decoder_stats().last_unknown_header;
  }

  this->publish_diag_uint(::atlantic_v5::ENT_FRAMES_OK, frames_ok);
  this->publish_diag_uint(::atlantic_v5::ENT_CRC_ERRORS, crc_errors);
  this->publish_diag_uint(::atlantic_v5::ENT_RESYNCS, resyncs);
  this->publish_diag_uint(::atlantic_v5::ENT_DROPPED_BYTES, dropped_bytes);
  this->publish_diag_uint(::atlantic_v5::ENT_UNKNOWN_FRAMES, unknown_frames);
  this->publish_diag_uint(::atlantic_v5::ENT_FRAMES_RELAYED, frames_relayed);
  this->publish_diag_uint(::atlantic_v5::ENT_REWRITES_APPLIED, rewrites_applied);
  this->publish_diag_uint(::atlantic_v5::ENT_ECHO_BYTES, echo_bytes);
  this->publish_diag_uint(::atlantic_v5::ENT_QUEUE_OVERFLOWS, queue_overflows);
  this->publish_diag_uint(::atlantic_v5::ENT_RELAY_LATENCY_MAX_US, latency_max_us);
  this->publish_diag_float(::atlantic_v5::ENT_RELAY_LATENCY_AVG_US, latency_avg_us);
  this->publish_diag_uint(::atlantic_v5::ENT_TASK_STACK_FREE, task_stack_free);

  // plan 3.8: "reset on read" - each published latency value covers only the
  // window since the previous diagnostics tick, not the time since boot.
  if (this->mode_ == Mode::MITM)
    this->relay_task_->reset_latency_stats();

  // last_unknown_frame: rate-limited to once/10s (plan 3.8), and only when a
  // *new* unknown header has actually appeared since the last time we looked.
  bool new_unknown = unknown_frames != this->last_unknown_headers_seen_;
  this->last_unknown_headers_seen_ = unknown_frames;
  if (!new_unknown || now_us - this->last_unknown_frame_us_ < 10'000'000)
    return;
  this->last_unknown_frame_us_ = now_us;

  void *obj = this->entities_[::atlantic_v5::ENT_LAST_UNKNOWN_FRAME];
  if (obj == nullptr || this->kinds_[::atlantic_v5::ENT_LAST_UNKNOWN_FRAME] != EntityKind::TEXT_SENSOR)
    return;
  char hex[::atlantic_v5::HEADER_LEN * 2 + 1];
  format_header_hex(last_unknown_header, hex);
  static_cast<text_sensor::TextSensor *>(obj)->publish_state(hex);
}

void AtlanticV5Component::maybe_dump_frame(const ::atlantic_v5::Frame &f, uint32_t t_us) {
  if (!this->raw_frame_dump_)
    return;
  if (t_us - this->last_frame_dump_us_ < 200'000)  // rate-limited (plan 3.8), 200ms
    return;
  this->last_frame_dump_us_ = t_us;

  void *obj = this->entities_[::atlantic_v5::ENT_LAST_FRAME_DUMP];
  if (obj == nullptr || this->kinds_[::atlantic_v5::ENT_LAST_FRAME_DUMP] != EntityKind::TEXT_SENSOR)
    return;
  char hex[::atlantic_v5::MAX_FRAME * 2 + 1];
  to_hex(f.raw(), f.raw_len(), hex);
  static_cast<text_sensor::TextSensor *>(obj)->publish_state(hex);
}

void AtlanticV5Component::frame_dump_trampoline(void *ctx, const ::atlantic_v5::Frame &f, uint32_t t_us) {
  static_cast<AtlanticV5Component *>(ctx)->maybe_dump_frame(f, t_us);
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
  bool is_stale = this->us_since_main(now_us) >= this->timeout_us_;
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

uint32_t AtlanticV5Component::us_since_main(uint32_t now_us) const {
#ifdef USE_ESP32
  if (this->mode_ == Mode::MITM)
    return now_us - this->last_main_us_;
#endif
  return this->listener_.us_since_main(now_us);
}

void AtlanticV5Component::dump_config() {
  ESP_LOGCONFIG(TAG, "Atlantic V5:");
  ESP_LOGCONFIG(TAG, "  Mode: %s", mode_ == Mode::MITM ? "mitm" : "listener");
  ESP_LOGCONFIG(TAG, "  Bus capture: %s", YESNO(bus_capture_));
  ESP_LOGCONFIG(TAG, "  Timeout: %" PRIu32 " ms", this->timeout_us_ / 1000);
}

}  // namespace atlantic_v5_component
}  // namespace esphome
