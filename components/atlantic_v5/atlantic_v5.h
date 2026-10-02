#pragma once

#include <cstddef>
#include <cstdint>

#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include "catalog.h"
#include "decoder.h"
#include "listener.h"
#include "relay.h"
#include "relay_policy.h"

#ifdef USE_ESP32
#include <driver/uart.h>

#include "bus_capture_logger.h"
#include "relay_task.h"
#include "uart_bus_io.h"
#endif

namespace esphome {
// Named atlantic_v5_component, not atlantic_v5: ESPHome's generated main.cpp
// does `using namespace esphome;` and never fully-qualifies its own codegen,
// so an unqualified reference to esphome::atlantic_v5::* would be ambiguous
// with the L1/L2 core's own global `namespace atlantic_v5` (catalog.h,
// listener.h, ...). See build-and-tooling notes.
namespace atlantic_v5_component {

// Fixed at YAML/compile time; there is deliberately no automatic mode detection.
enum class Mode : uint8_t { LISTENER, MITM };

// Which ESPHome platform an entities_[] slot holds. Only
// read-only entities register through this generic path (ADR 0001); the
// control_mode select and log_raw_frames switch are controllable and get
// their own dedicated classes (atlantic_v5_select.h, atlantic_v5_switch.h).
enum class EntityKind : uint8_t { SENSOR, BINARY_SENSOR, TEXT_SENSOR };

class AtlanticV5Component : public Component {
 public:
  void set_mode(Mode mode) { mode_ = mode; }
  void set_bus_capture(bool enabled) { bus_capture_ = enabled; }
  void set_uart_num(int uart_num) { uart_num_ = uart_num; }
  void set_rx_pin(int rx_pin) { rx_pin_ = rx_pin; }
  // timeout_ms is stored in microseconds so it compares directly against
  // Listener::us_since_main() without a conversion on every loop() tick.
  void set_timeout(uint32_t timeout_ms) { timeout_us_ = timeout_ms * 1000ULL; }

  // Bus timing knobs. frame_silence is the framing backstop used
  // by every assembler in both modes.
  void set_frame_silence(uint32_t us) { frame_silence_us_ = us; }

  // MITM-mode side wiring. tx_enable_pin -1 means no DIR pin;
  // one_wire_mirror selects the shared-pin transceiver. Called once per side from
  // to_code.
  void set_hmi_uart(int uart_num, int rx_pin, int tx_pin, int tx_enable_pin, bool one_wire_mirror,
                    uint32_t dir_setup_us, uint32_t dir_hold_us);
  void set_main_uart(int uart_num, int rx_pin, int tx_pin, int tx_enable_pin, bool one_wire_mirror,
                     uint32_t dir_setup_us, uint32_t dir_hold_us);
  void set_relay_core(int core) { relay_core_ = core; }
  void set_forward_bad_crc(bool enabled) { forward_bad_crc_ = enabled; }

  // Registers a read-only entity for EntityId id (catalog.h). obj must
  // outlive this component; kind selects which publish_state overload to call.
  void set_entity(uint16_t id, void *obj, EntityKind kind);

  // The control_mode select's write path: writes into policy_,
  // which only Relay (mitm-only) ever reads, so it has no effect in listener
  // mode - unreachable anyway, since select.py rejects control_mode there.
  void set_control_mode(::atlantic_v5::ControlMode mode);

  // The log_raw_frames switch's write path: gates whether
  // already-framed, CRC-valid frames get hex-logged at DEBUG level.
  void set_log_raw_frames(bool enabled) { this->log_raw_frames_ = enabled; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  static void publish_trampoline(void *ctx, const ::atlantic_v5::DecodedValue &v);
  void publish(const ::atlantic_v5::DecodedValue &v);
  void update_staleness(uint32_t now_us);
  uint32_t us_since_main(uint32_t now_us) const;
  // Whether a payload-bearing MAIN frame has been seen at all since boot. Until
  // one has, us_since_main() is just "microseconds since boot" and says nothing.
  bool has_main() const;

#ifdef USE_ESP32
  // Listener-mode UART bring-up, also feeding bus_capture.
  uart_port_t port_{UART_NUM_1};

  void setup_listener();
  void loop_listener();
  void setup_mitm();
  void loop_mitm();
  static void capture_sink_trampoline(void *ctx, ::atlantic_v5::Channel channel, const uint8_t *data, size_t len,
                                       uint32_t t_us);

  // Diagnostic counters, rate-limited to once/second; source stats
  // differ by mode (RelayTask/Relay/FrameAssembler in mitm, Listener in
  // listener), published through the same dedup'd publish() as decoded values.
  void update_diagnostics(uint32_t now_us);
  void publish_diag_uint(uint16_t id, uint32_t value);
  void publish_diag_float(uint16_t id, float value);

  // log_raw_frames: fired for every already-framed, CRC-valid frame
  // in both modes - directly from loop_mitm()'s decoded FrameEvents, and via
  // Listener::FrameSink in listener mode (frame_log_trampoline).
  void maybe_log_frame(const ::atlantic_v5::Frame &f);
  static void frame_log_trampoline(void *ctx, const ::atlantic_v5::Frame &f, uint32_t t_us);

  // Per-side MITM config; unused in listener mode.
  struct SideConfig {
    int uart_num = -1;
    int rx_pin = -1;
    int tx_pin = -1;
    int tx_enable_pin = -1;
    bool one_wire_mirror = false;
    uint32_t dir_setup_us = ::atlantic_v5::DEFAULT_DIR_SETUP_US;
    uint32_t dir_hold_us = ::atlantic_v5::DEFAULT_DIR_HOLD_US;
  };
  SideConfig hmi_cfg_;
  SideConfig main_cfg_;
  int relay_core_{1};
  bool forward_bad_crc_{false};

  ::atlantic_v5::RelayPolicy policy_;
  ::atlantic_v5::RelayTask *relay_task_{nullptr};
  // MITM frames arrive already-framed via RelayTask's FrameEvent queue, decoded
  // here directly rather than through Listener (which owns its own single-bus
  // assembler this path doesn't need).
  ::atlantic_v5::Decoder decoder_;
  uint32_t last_main_us_{0};
  bool seen_main_{false};

  BusCaptureLogger *hmi_capture_logger_{nullptr};
  BusCaptureLogger *main_capture_logger_{nullptr};
  BusCaptureLogger capture_logger_{"bus"};  // listener mode's single tapped wire

  uint32_t last_diag_us_{0};
#endif
  Mode mode_{Mode::LISTENER};
  bool bus_capture_{false};
  bool log_raw_frames_{false};
  int uart_num_{1};
  int rx_pin_{-1};
  uint32_t timeout_us_{60'000'000};  // default 60s
  uint32_t frame_silence_us_{::atlantic_v5::FrameAssembler::DEFAULT_SILENCE_US};

  ::atlantic_v5::Listener listener_;
  void *entities_[::atlantic_v5::ENT_COUNT]{};
  EntityKind kinds_[::atlantic_v5::ENT_COUNT]{};
  // Last published numeric value per entity, for the "only publish when
  // changed, or force_update" rule. binary_sensor/text_sensor
  // already dedupe internally in ESPHome core; sensor::Sensor does not.
  float last_published_[::atlantic_v5::ENT_COUNT];
  bool has_last_published_[::atlantic_v5::ENT_COUNT]{};
  bool stale_{false};
};


}  // namespace atlantic_v5_component
}  // namespace esphome
