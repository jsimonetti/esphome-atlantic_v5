#pragma once

#include <cstddef>
#include <cstdint>

#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include "catalog.h"
#include "listener.h"

#ifdef USE_ESP32
#include <driver/uart.h>
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
// select is controllable and gets its own dedicated class at ticket 09.
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

  // Registers a read-only entity for EntityId id (core/catalog.h). obj must
  // outlive this component; kind selects which publish_state overload to call.
  void set_entity(uint16_t id, void *obj, EntityKind kind);

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  static void publish_trampoline(void *ctx, const ::atlantic_v5::DecodedValue &v);
  void publish(const ::atlantic_v5::DecodedValue &v);
  void update_staleness(uint32_t now_us);

#ifdef USE_ESP32
  // M0.5: minimal listener-mode UART bring-up feeding bus_capture (3.5.5).
  // Folds into transport/uart_bus_io.* once M6 builds the full BusIo layer.
  uart_port_t port_{UART_NUM_1};

  // Frames are variable-length (plan 2.3), so bus_capture accumulates bytes
  // across loop() calls and flushes on the plan's 4 ms silence backstop
  // (2.1/2.5.1) rather than logging whatever a single loop() tick read.
  static constexpr size_t CAPTURE_BUF_LEN = 32;  // matches the documented max frame size
  uint8_t capture_buf_[CAPTURE_BUF_LEN]{};
  size_t capture_len_{0};
  int64_t capture_last_byte_us_{0};

  void capture_bytes(const uint8_t *chunk, size_t len, int64_t now_us);
#endif
  Mode mode_{Mode::LISTENER};
  bool bus_capture_{false};
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
