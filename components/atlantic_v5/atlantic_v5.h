#pragma once

#include <cstddef>

#include "esphome/core/component.h"

#ifdef USE_ESP32
#include <driver/uart.h>
#endif

namespace esphome {
namespace atlantic_v5 {

// Fixed at YAML/compile time (see implementation plan, non-goals: no automatic
// mode detection).
enum class Mode : uint8_t { LISTENER, MITM };

class AtlanticV5Component : public Component {
 public:
  void set_mode(Mode mode) { mode_ = mode; }
  void set_bus_capture(bool enabled) { bus_capture_ = enabled; }
  void set_uart_num(int uart_num) { uart_num_ = uart_num; }
  void set_rx_pin(int rx_pin) { rx_pin_ = rx_pin; }
  void set_tx_pin(int tx_pin) { tx_pin_ = tx_pin; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
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
#endif
  Mode mode_{Mode::LISTENER};
  bool bus_capture_{false};
  int uart_num_{1};
  int rx_pin_{-1};
  int tx_pin_{-1};
};

}  // namespace atlantic_v5
}  // namespace esphome
