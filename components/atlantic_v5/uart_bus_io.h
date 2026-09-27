// L2 transport, esp-idf only (plan 3.5.2-3.5.3). The firmware-side BusIo: UART
// bring-up plus all three DIR/one-wire hardware cases. Host tests exercise Relay
// against MockBusIo instead (transport/bus_io.h); this file is never part of the
// host CMake build (see test/host/CMakeLists.txt).
#pragma once

#ifdef USE_ESP32

#include <driver/gpio.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "bus_io.h"
#include "types.h"

namespace atlantic_v5 {

inline constexpr uint32_t DEFAULT_DIR_SETUP_US = 10;   // plan 3.5.3 case A default
inline constexpr uint32_t DEFAULT_DIR_HOLD_US = 260;  // plan 3.5.3 case A default (~1 byte time)

struct UartBusIoConfig {
  uart_port_t port = UART_NUM_1;
  int rx_pin = -1;
  int tx_pin = -1;
  int tx_enable_pin = -1;    // DIR pin, -1 = none (plan 3.5.3 case B)
  bool one_wire_mirror = false;  // plan 3.5.3 case C
  uint32_t dir_setup_us = DEFAULT_DIR_SETUP_US;
  uint32_t dir_hold_us = DEFAULT_DIR_HOLD_US;
};

class UartBusIo : public BusIo {
 public:
  explicit UartBusIo(const UartBusIoConfig &cfg) : cfg_(cfg) {}

  // Configures the UART port and DIR/mirror pins and parks the line in RX
  // (plan 3.5.3: "Do the same parking at setup time so the pins are never
  // driven while idle"). Must be called from Component::setup(), never from a
  // constructor (esp-idf drivers aren't safe to touch at static-init time).
  void install();

  int read(uint8_t *dst, size_t max, uint32_t timeout_us) override;
  void write(const uint8_t *src, size_t len) override;
  void flush_input() override;
  uint32_t now_us() override;

  // For RelayTask's QueueSet (plan 3.6.3's "wait on both from one task"
  // alternative): the UART driver's own event queue, valid after install().
  QueueHandle_t event_queue() const { return event_queue_; }

 private:
  // Plan 3.5.3's single hardware-tuning function: only meaningful when
  // one_wire_mirror is set (reroutes the TX signal onto both pins during TX,
  // parks both as inputs during RX). Cases A/B (no mirror) are a no-op here;
  // their DIR-pin assert/hold/deassert lives directly in write(), where the
  // exact timing relative to uart_write_bytes/uart_wait_tx_done matters.
  void set_line_mode(LineMode mode);

  UartBusIoConfig cfg_;
  QueueHandle_t event_queue_ = nullptr;
  uint32_t tx_sig_ = 0;
  uint32_t rx_sig_ = 0;
};

}  // namespace atlantic_v5

#endif  // USE_ESP32
