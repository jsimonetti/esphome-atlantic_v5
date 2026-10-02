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
// conventions elsewhere.
void format_header_hex(uint64_t key, char *out) {
  uint8_t bytes[::atlantic_v5::HEADER_LEN];
  for (size_t i = 0; i < ::atlantic_v5::HEADER_LEN; i++)
    bytes[i] = static_cast<uint8_t>(key >> ((::atlantic_v5::HEADER_LEN - 1 - i) * 8));
  to_hex(bytes, sizeof(bytes), out);
}

const char *channel_name(::atlantic_v5::Channel ch) {
  switch (ch) {
    case ::atlantic_v5::Channel::HMI:
      return "HMI";
    case ::atlantic_v5::Channel::MAIN:
      return "MAIN";
    default:
      return "BUS";
  }
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
  // TX stays unassigned (G2): a transceiver parked in receive drives that pin.
  uart_set_pin(this->port_, UART_PIN_NO_CHANGE, this->rx_pin_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  uart_driver_install(this->port_, 512, 0, 0, nullptr, 0);
  uart_set_rx_full_threshold(this->port_, 1);
  uart_set_rx_timeout(this->port_, 2);

  this->listener_.set_silence_us(this->frame_silence_us_);
  this->listener_.set_sink(&AtlanticV5Component::publish_trampoline, this);
  this->listener_.set_frame_sink(&AtlanticV5Component::frame_log_trampoline, this);
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

void AtlanticV5Component::set_hmi_uart(int uart_num, int rx_pin, int tx_pin, int tx_enable_pin, bool one_wire_mirror,
                                       uint32_t dir_setup_us, uint32_t dir_hold_us) {
  this->hmi_cfg_ = {uart_num, rx_pin, tx_pin, tx_enable_pin, one_wire_mirror, dir_setup_us, dir_hold_us};
}

void AtlanticV5Component::set_main_uart(int uart_num, int rx_pin, int tx_pin, int tx_enable_pin,
                                         bool one_wire_mirror, uint32_t dir_setup_us, uint32_t dir_hold_us) {
  this->main_cfg_ = {uart_num, rx_pin, tx_pin, tx_enable_pin, one_wire_mirror, dir_setup_us, dir_hold_us};
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
  hmi_io_cfg.dir_setup_us = this->hmi_cfg_.dir_setup_us;
  hmi_io_cfg.dir_hold_us = this->hmi_cfg_.dir_hold_us;

  ::atlantic_v5::UartBusIoConfig main_io_cfg{};
  main_io_cfg.port = static_cast<uart_port_t>(this->main_cfg_.uart_num);
  main_io_cfg.rx_pin = this->main_cfg_.rx_pin;
  main_io_cfg.tx_pin = this->main_cfg_.tx_pin;
  main_io_cfg.tx_enable_pin = this->main_cfg_.tx_enable_pin;
  main_io_cfg.one_wire_mirror = this->main_cfg_.one_wire_mirror;
  main_io_cfg.dir_setup_us = this->main_cfg_.dir_setup_us;
  main_io_cfg.dir_hold_us = this->main_cfg_.dir_hold_us;

  ::atlantic_v5::RelayTask::Config cfg{};
  cfg.hmi = hmi_io_cfg;
  cfg.main = main_io_cfg;
  cfg.relay.silence_us = this->frame_silence_us_;
  cfg.relay.forward_bad_crc = this->forward_bad_crc_;
  cfg.relay_core = this->relay_core_;

  this->relay_task_ = new ::atlantic_v5::RelayTask(cfg, this->policy_);
  if (this->bus_capture_) {
    this->hmi_capture_logger_ = new BusCaptureLogger("hmi");
    this->main_capture_logger_ = new BusCaptureLogger("main");
    this->relay_task_->set_capture_sink(&AtlanticV5Component::capture_sink_trampoline, this);
  }
  this->relay_task_->begin();
}

void AtlanticV5Component::loop_mitm() {
  ::atlantic_v5::FrameEvent events[8];  // N=8 per iteration, bounds loop time
  size_t n = this->relay_task_->drain_events(events, 8);
  for (size_t i = 0; i < n; i++) {
    const auto &ev = events[i];
    if (!ev.crc_ok)
      continue;  // only reaches here at all under forward_bad_crc; never decoded
    ::atlantic_v5::Frame f(ev.channel, ev.data, ev.len);
    // Transaction-byte rule: only MAIN's payload-bearing response
    // resets the staleness gate, matching listener mode's semantics.
    if (ev.channel == ::atlantic_v5::Channel::MAIN && f.has_payload()) {
      this->last_main_us_ = ev.t_us;
      this->seen_main_ = true;
    }
    // Log before restoring: log_raw_frames exists to show what went out on the
    // wire, which is the rewritten frame.
    this->maybe_log_frame(f);
    // ADR 0002: the input entities report the observed input, so put the
    // pre-rewrite payload back before decoding.
    if (ev.modified)
      f.replace_payload(ev.observed_payload);
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
  if (now_us - this->last_diag_us_ < 1'000'000)  // 1s matches the bus's own rate
    return;
  this->last_diag_us_ = now_us;

  uint32_t frames_relayed = 0, rewrites_applied = 0, queue_overflows = 0;
  uint32_t latency_max_us = 0, task_stack_free = 0;
  float latency_avg_us = 0;
  const ::atlantic_v5::Decoder::Stats *dec_stats;

  if (this->mode_ == Mode::MITM) {
    // Published per side and never summed: a sum cannot distinguish "both sides
    // healthy" from "one side dead, the other noisy".
    const auto &hmi = this->relay_task_->hmi_stats();
    const auto &main = this->relay_task_->main_stats();
    this->publish_diag_uint(::atlantic_v5::ENT_VALID_FRAMES_HMI, hmi.valid_frames);
    this->publish_diag_uint(::atlantic_v5::ENT_CRC_ERRORS_HMI, hmi.crc_errors);
    this->publish_diag_uint(::atlantic_v5::ENT_DROPPED_BYTES_HMI, hmi.dropped_bytes);
    this->publish_diag_uint(::atlantic_v5::ENT_VALID_FRAMES_MAIN, main.valid_frames);
    this->publish_diag_uint(::atlantic_v5::ENT_CRC_ERRORS_MAIN, main.crc_errors);
    this->publish_diag_uint(::atlantic_v5::ENT_DROPPED_BYTES_MAIN, main.dropped_bytes);
    dec_stats = &this->decoder_.stats();

    const auto &relay_stats = this->relay_task_->stats();
    frames_relayed = relay_stats.frames_relayed;
    rewrites_applied = relay_stats.rewrites_applied;
    latency_max_us = relay_stats.latency_max_us;
    if (relay_stats.latency_samples > 0)
      latency_avg_us = static_cast<float>(relay_stats.latency_total_us) / static_cast<float>(relay_stats.latency_samples);
    queue_overflows = this->relay_task_->queue_overflows();
    task_stack_free = this->relay_task_->stack_high_water_mark();
  } else {
    // One wire, one assembler: the unsuffixed counters already describe a side.
    const auto &asm_stats = this->listener_.assembler_stats();
    this->publish_diag_uint(::atlantic_v5::ENT_VALID_FRAMES, asm_stats.valid_frames);
    this->publish_diag_uint(::atlantic_v5::ENT_CRC_ERRORS, asm_stats.crc_errors);
    this->publish_diag_uint(::atlantic_v5::ENT_DROPPED_BYTES, asm_stats.dropped_bytes);
    dec_stats = &this->listener_.decoder_stats();
  }

  this->publish_diag_uint(::atlantic_v5::ENT_UNKNOWN_FRAMES, dec_stats->unknown_headers);
  this->publish_diag_uint(::atlantic_v5::ENT_FRAMES_RELAYED, frames_relayed);
  this->publish_diag_uint(::atlantic_v5::ENT_REWRITES_APPLIED, rewrites_applied);
  this->publish_diag_uint(::atlantic_v5::ENT_QUEUE_OVERFLOWS, queue_overflows);
  this->publish_diag_uint(::atlantic_v5::ENT_RELAY_LATENCY_MAX_US, latency_max_us);
  this->publish_diag_float(::atlantic_v5::ENT_RELAY_LATENCY_AVG_US, latency_avg_us);
  this->publish_diag_uint(::atlantic_v5::ENT_TASK_STACK_FREE, task_stack_free);

  // Latency stats are reset on read: each published value covers only the
  // window since the previous diagnostics tick, not the time since boot.
  if (this->mode_ == Mode::MITM)
    this->relay_task_->reset_latency_stats();

  // last_unknown_frame: rate-limited to once/10s, and only when a
  // *new* unknown header has actually appeared since the last time we looked.
  // Unmapped headers (docs/protocol.md) never reach this counter, so it only
  // moves for traffic we have genuinely never seen before.
  bool new_unknown = dec_stats->unknown_headers != this->last_unknown_headers_seen_;
  this->last_unknown_headers_seen_ = dec_stats->unknown_headers;
  if (!new_unknown || now_us - this->last_unknown_frame_us_ < 10'000'000)
    return;
  this->last_unknown_frame_us_ = now_us;

  void *obj = this->entities_[::atlantic_v5::ENT_LAST_UNKNOWN_FRAME];
  if (obj == nullptr || this->kinds_[::atlantic_v5::ENT_LAST_UNKNOWN_FRAME] != EntityKind::TEXT_SENSOR)
    return;
  // Header hex, then a separating space, then payload hex, then the NUL.
  char hex[(::atlantic_v5::HEADER_LEN + ::atlantic_v5::MAX_PAYLOAD) * 2 + 2];
  format_header_hex(dec_stats->last_unknown_header, hex);
  // The payload is what actually identifies a new message, but it is only
  // useful while someone is watching, so it rides on the log_raw_frames switch
  // rather than on a config key of its own.
  if (this->log_raw_frames_ && dec_stats->last_unknown_payload_len > 0) {
    size_t n = ::atlantic_v5::HEADER_LEN * 2;
    hex[n++] = ' ';
    to_hex(dec_stats->last_unknown_payload, dec_stats->last_unknown_payload_len, hex + n);
  }
  static_cast<text_sensor::TextSensor *>(obj)->publish_state(hex);
}

void AtlanticV5Component::maybe_log_frame(const ::atlantic_v5::Frame &f) {
  if (!this->log_raw_frames_)
    return;

  char hex[::atlantic_v5::MAX_FRAME * 2 + 1];
  to_hex(f.raw(), f.raw_len(), hex);
  ESP_LOGD(TAG, "frame %s %s", channel_name(f.channel()), hex);
}

void AtlanticV5Component::frame_log_trampoline(void *ctx, const ::atlantic_v5::Frame &f, uint32_t t_us) {
  (void) t_us;
  static_cast<AtlanticV5Component *>(ctx)->maybe_log_frame(f);
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
  // The one place that decides "stale": the NAN publish, the
  // component warning and the `connected` entity are three presentations of
  // this single result, never three clocks.
  bool is_stale = !this->has_main() || this->us_since_main(now_us) >= this->timeout_us_;

  // Published unconditionally rather than only on a transition: the first tick
  // has to establish the initial state, and binary_sensor::publish_state()
  // dedupes by value itself.
  void *connected = this->entities_[::atlantic_v5::ENT_CONNECTED];
  if (connected != nullptr && this->kinds_[::atlantic_v5::ENT_CONNECTED] == EntityKind::BINARY_SENSOR)
    static_cast<binary_sensor::BinarySensor *>(connected)->publish_state(!is_stale);

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

bool AtlanticV5Component::has_main() const {
#ifdef USE_ESP32
  if (this->mode_ == Mode::MITM)
    return this->seen_main_;
#endif
  return this->listener_.has_main();
}

void AtlanticV5Component::dump_config() {
  ESP_LOGCONFIG(TAG, "Atlantic V5:");
  ESP_LOGCONFIG(TAG, "  Mode: %s", mode_ == Mode::MITM ? "mitm" : "listener");
  ESP_LOGCONFIG(TAG, "  Bus capture: %s", YESNO(bus_capture_));
  ESP_LOGCONFIG(TAG, "  Timeout: %" PRIu32 " ms", this->timeout_us_ / 1000);
}

}  // namespace atlantic_v5_component
}  // namespace esphome
