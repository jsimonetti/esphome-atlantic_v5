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

// Fixed at YAML/compile time (see implementation plan, non-goals: no automatic
// mode detection).
enum class Mode : uint8_t { LISTENER, MITM };

// Which ESPHome platform an entities_[] slot holds (plan 3.7.1/3.7.3). Only
// read-only entities register through this generic path (ADR 0001); the
// control_mode select and raw_frame_dump switch are controllable and get
// their own dedicated classes (atlantic_v5_select.h, atlantic_v5_switch.h).
enum class EntityKind : uint8_t { SENSOR, BINARY_SENSOR, TEXT_SENSOR };

class AtlanticV5Component : public Component {
 public:
  void set_mode(Mode mode) { mode_ = mode; }
  void set_bus_capture(bool enabled) { bus_capture_ = enabled; }
  void set_uart_num(int uart_num) { uart_num_ = uart_num; }
  void set_rx_pin(int rx_pin) { rx_pin_ = rx_pin; }
  void set_tx_pin(int tx_pin) { tx_pin_ = tx_pin; }
  // timeout_ms is stored in microseconds so it compares directly against
  // Listener::us_since_main() without a conversion on every loop() tick.
  void set_timeout(uint32_t timeout_ms) { timeout_us_ = timeout_ms * 1000ULL; }

  // MITM-mode side wiring (plan 3.7.2/3.5.3). tx_enable_pin -1 means no DIR pin
  // (case B); one_wire_mirror selects case C. Called once per side from to_code.
  void set_hmi_uart(int uart_num, int rx_pin, int tx_pin, int tx_enable_pin, bool one_wire_mirror);
  void set_main_uart(int uart_num, int rx_pin, int tx_pin, int tx_enable_pin, bool one_wire_mirror);
  void set_relay_core(int core) { relay_core_ = core; }
  void set_self_test(bool enabled) { self_test_ = enabled; }

  // Registers a read-only entity for EntityId id (core/catalog.h). obj must
  // outlive this component; kind selects which publish_state overload to call.
  void set_entity(uint16_t id, void *obj, EntityKind kind);

  // The control_mode select's write path (plan 2.8/3.4): writes into policy_,
  // which only Relay (mitm-only) ever reads, so it has no effect in listener
  // mode - unreachable anyway, since select.py rejects control_mode there.
  void set_control_mode(::atlantic_v5::ControlMode mode);

  // The raw_frame_dump switch's write path (plan 3.8): gates whether
  // already-framed, already-decoded frames get hex-dumped to a text sensor.
  void set_raw_frame_dump(bool enabled) { this->raw_frame_dump_ = enabled; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  static void publish_trampoline(void *ctx, const ::atlantic_v5::DecodedValue &v);
  void publish(const ::atlantic_v5::DecodedValue &v);
  void update_staleness(uint32_t now_us);
  uint32_t us_since_main(uint32_t now_us) const;

#ifdef USE_ESP32
  // M0.5: minimal listener-mode UART bring-up feeding bus_capture (3.5.5).
  uart_port_t port_{UART_NUM_1};

  void setup_listener();
  void loop_listener();
  void setup_mitm();
  void loop_mitm();
  static void capture_sink_trampoline(void *ctx, ::atlantic_v5::Channel channel, const uint8_t *data, size_t len,
                                       uint32_t t_us);

  // Diagnostic counters (plan 3.8), rate-limited to once/second; source stats
  // differ by mode (RelayTask/Relay/FrameAssembler in mitm, Listener in
  // listener), published through the same dedup'd publish() as decoded values.
  void update_diagnostics(uint32_t now_us);
  void publish_diag_uint(uint16_t id, uint32_t value);
  void publish_diag_float(uint16_t id, float value);

  // raw_frame_dump (plan 3.8): fired for every already-framed, CRC-valid frame
  // in both modes - directly from loop_mitm()'s decoded FrameEvents, and via
  // Listener::FrameSink in listener mode (frame_dump_trampoline).
  void maybe_dump_frame(const ::atlantic_v5::Frame &f, uint32_t t_us);
  static void frame_dump_trampoline(void *ctx, const ::atlantic_v5::Frame &f, uint32_t t_us);

  // Per-side MITM config (plan 3.7.2); unused in listener mode.
  struct SideConfig {
    int uart_num = -1;
    int rx_pin = -1;
    int tx_pin = -1;
    int tx_enable_pin = -1;
    bool one_wire_mirror = false;
  };
  SideConfig hmi_cfg_;
  SideConfig main_cfg_;
  int relay_core_{1};
  bool self_test_{true};

  ::atlantic_v5::RelayPolicy policy_;
  ::atlantic_v5::RelayTask *relay_task_{nullptr};
  // MITM frames arrive already-framed via RelayTask's FrameEvent queue, decoded
  // here directly rather than through Listener (which owns its own single-bus
  // assembler this path doesn't need).
  ::atlantic_v5::Decoder decoder_;
  uint32_t last_main_us_{0};

  BusCaptureLogger *hmi_capture_logger_{nullptr};
  BusCaptureLogger *main_capture_logger_{nullptr};
  BusCaptureLogger capture_logger_{"bus"};  // listener mode's single tapped wire

  uint32_t last_diag_us_{0};
  uint32_t last_unknown_frame_us_{0};
  uint32_t last_unknown_headers_seen_{0};
  uint32_t last_frame_dump_us_{0};
#endif
  Mode mode_{Mode::LISTENER};
  bool bus_capture_{false};
  bool raw_frame_dump_{false};
  int uart_num_{1};
  int rx_pin_{-1};
  int tx_pin_{-1};
  uint32_t timeout_us_{60'000'000};  // plan 3.7.1 default 60s

  ::atlantic_v5::Listener listener_;
  void *entities_[::atlantic_v5::ENT_COUNT]{};
  EntityKind kinds_[::atlantic_v5::ENT_COUNT]{};
  // Last published numeric value per entity, for the "only publish when
  // changed, or force_update" rule (plan 3.7.1). binary_sensor/text_sensor
  // already dedupe internally in ESPHome core; sensor::Sensor does not.
  float last_published_[::atlantic_v5::ENT_COUNT];
  bool has_last_published_[::atlantic_v5::ENT_COUNT]{};
  bool stale_{false};
};


}  // namespace atlantic_v5_component
}  // namespace esphome
