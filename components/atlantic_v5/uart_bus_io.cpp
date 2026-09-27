#include "uart_bus_io.h"

#ifdef USE_ESP32

#include <esp_rom_gpio.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <soc/gpio_sig_map.h>
#include <soc/uart_periph.h>

namespace atlantic_v5 {

void UartBusIo::install() {
  uart_config_t cfg{};
  cfg.baud_rate = static_cast<int>(BAUD);
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;
  uart_param_config(this->cfg_.port, &cfg);
  uart_set_pin(this->cfg_.port, this->cfg_.tx_pin, this->cfg_.rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  uart_driver_install(this->cfg_.port, /*rx_buf*/ 512, /*tx_buf*/ 0, /*queue*/ 16, &this->event_queue_, 0);
  uart_set_rx_full_threshold(this->cfg_.port, 1);
  uart_set_rx_timeout(this->cfg_.port, 2);

  if (this->cfg_.tx_enable_pin >= 0) {
    gpio_set_direction(static_cast<gpio_num_t>(this->cfg_.tx_enable_pin), GPIO_MODE_OUTPUT);
    gpio_set_level(static_cast<gpio_num_t>(this->cfg_.tx_enable_pin), 0);  // idle LOW (receive)
  }

  if (this->cfg_.one_wire_mirror) {
    // Plan 3.5.3: tx_sig/rx_sig = uart_periph_signal[port].pins[SOC_UART_{TX,RX}_PIN_IDX].signal.
    this->tx_sig_ = uart_periph_signal[this->cfg_.port].pins[SOC_UART_TX_PIN_IDX].signal;
    this->rx_sig_ = uart_periph_signal[this->cfg_.port].pins[SOC_UART_RX_PIN_IDX].signal;
  }

  this->set_line_mode(LineMode::RX);  // park at setup time too, never driven while idle
}

int UartBusIo::read(uint8_t *dst, size_t max, uint32_t timeout_us) {
  // uart_read_bytes takes ticks, not microseconds; round up so a caller-requested
  // wait is never truncated to zero by integer division.
  TickType_t ticks = timeout_us == 0 ? 0 : pdMS_TO_TICKS((timeout_us + 999) / 1000);
  int n = uart_read_bytes(this->cfg_.port, dst, max, ticks);
  return n < 0 ? 0 : n;
}

void UartBusIo::write(const uint8_t *src, size_t len) {
  this->set_line_mode(LineMode::TX);
  if (this->cfg_.tx_enable_pin >= 0) {
    gpio_set_level(static_cast<gpio_num_t>(this->cfg_.tx_enable_pin), 1);
    esp_rom_delay_us(this->cfg_.dir_setup_us);
  }

  uart_write_bytes(this->cfg_.port, reinterpret_cast<const char *>(src), len);
  uart_wait_tx_done(this->cfg_.port, portMAX_DELAY);

  if (this->cfg_.tx_enable_pin >= 0) {
    esp_rom_delay_us(this->cfg_.dir_hold_us);
    gpio_set_level(static_cast<gpio_num_t>(this->cfg_.tx_enable_pin), 0);
  }
  this->set_line_mode(LineMode::RX);
}

void UartBusIo::flush_input() { uart_flush_input(this->cfg_.port); }

uint32_t UartBusIo::now_us() { return static_cast<uint32_t>(esp_timer_get_time()); }

void UartBusIo::set_line_mode(LineMode mode) {
  if (!this->cfg_.one_wire_mirror)
    return;  // cases A/B: DIR pin (if any) is handled directly in write()

  gpio_num_t tx_pin = static_cast<gpio_num_t>(this->cfg_.tx_pin);
  gpio_num_t rx_pin = static_cast<gpio_num_t>(this->cfg_.rx_pin);

  if (mode == LineMode::TX) {
    // Mirror the TX signal onto both pins so it reaches the bus regardless of
    // which transceiver channel is physically wired to it (plan 3.5.3 case C).
    gpio_set_direction(tx_pin, GPIO_MODE_OUTPUT);
    esp_rom_gpio_connect_out_signal(tx_pin, this->tx_sig_, false, false);
    gpio_set_direction(rx_pin, GPIO_MODE_INPUT_OUTPUT);
    esp_rom_gpio_connect_out_signal(rx_pin, this->tx_sig_, false, false);
    esp_rom_delay_us(10);
  } else {
    // Park both pins as plain inputs and reattach the UART's own RX signal so
    // neither pin is driven while idle.
    esp_rom_gpio_connect_out_signal(tx_pin, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_direction(tx_pin, GPIO_MODE_INPUT);
    esp_rom_gpio_connect_out_signal(rx_pin, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_direction(rx_pin, GPIO_MODE_INPUT);
    esp_rom_gpio_connect_in_signal(rx_pin, this->rx_sig_, false);
  }
}

}  // namespace atlantic_v5

#endif  // USE_ESP32
