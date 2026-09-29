// L2 transport, esp-idf only. The firmware-side BusIo: UART
// bring-up plus all three DIR/one-wire hardware cases. Host tests exercise Relay
// against MockBusIo instead (bus_io.h); this file is never part of the
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

inline constexpr uint32_t DEFAULT_DIR_SETUP_US = 10;
// 0 matches the one known-good V5 relay on this hardware: it drops DIR straight
// after flush. Holding longer keeps the transceiver driving the shared wire and
// can clip the start of the peer's reply.
inline constexpr uint32_t DEFAULT_DIR_HOLD_US = 0;

struct UartBusIoConfig {
  uart_port_t port = UART_NUM_1;
  int rx_pin = -1;
  int tx_pin = -1;
  int tx_enable_pin = -1;    // DIR pin, -1 = none
  bool one_wire_mirror = false;
  uint32_t dir_setup_us = DEFAULT_DIR_SETUP_US;
  uint32_t dir_hold_us = DEFAULT_DIR_HOLD_US;
};

class UartBusIo : public BusIo {
 public:
  explicit UartBusIo(const UartBusIoConfig &cfg) : cfg_(cfg) {}

  // Configures the UART port and DIR/mirror pins and parks the line in RX, so the
  // pins are never driven while idle. Must be called from Component::setup(), never
  // from a constructor (esp-idf drivers aren't safe to touch at static-init time).
  void install();

  int read(uint8_t *dst, size_t max, uint32_t timeout_us) override;
  void write(const uint8_t *src, size_t len) override;
  void flush_input() override;
  uint32_t now_us() override;

  // For RelayTask's QueueSet, so one task can wait on both sides: the UART
  // driver's own event queue, valid after install().
  QueueHandle_t event_queue() const { return event_queue_; }

 private:
  // The single hardware-tuning function: only meaningful when
  // one_wire_mirror is set (reroutes the TX signal onto both pins during TX,
  // parks both as inputs during RX). Without the mirror it is a no-op here;
  // the DIR-pin assert/hold/deassert lives directly in write(), where the
  // exact timing relative to uart_write_bytes/uart_wait_tx_done matters.
  void set_line_mode(LineMode mode);

  UartBusIoConfig cfg_;
  QueueHandle_t event_queue_ = nullptr;
  uint32_t tx_sig_ = 0;
  uint32_t rx_sig_ = 0;
};

}  // namespace atlantic_v5

#endif  // USE_ESP32
