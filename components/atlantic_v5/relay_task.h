// L2 transport, esp-idf only (plan 3.6). Owns the two UartBusIo sides and the
// hardware-free Relay (M5) together, and is the only place FreeRTOS task/queue
// specifics live — Relay itself stays host-testable with MockBusIo.
#pragma once

#ifdef USE_ESP32

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "relay.h"
#include "relay_policy.h"
#include "types.h"
#include "uart_bus_io.h"

namespace atlantic_v5 {

class RelayTask {
 public:
  struct Config {
    UartBusIoConfig hmi;
    UartBusIoConfig main;
    Relay::Config relay{};
    int relay_core = 1;    // plan 3.6.3; -1 == tskNO_AFFINITY (single-core targets)
    bool self_test = true;  // plan 3.5.4
  };

  RelayTask(const Config &cfg, RelayPolicy &policy);

  // Optional, must be called before begin() (plan 3.5.5 MITM piggyback): raw
  // pre-assembly bytes read by the relay task are handed to sink as a byproduct,
  // tagged with the physical side they came from.
  void set_capture_sink(Relay::CaptureSink sink, void *ctx) { this->relay_.set_capture_sink(sink, ctx); }

  // Brings up both UARTs, runs the startup self-test (3.5.4), and spawns the
  // pinned relay task (3.6.3). Call once from Component::setup().
  void begin();

  // Drains up to max_events completed frames into out (plan 3.6.5: "drain up to
  // N events per iteration"). Call only from the main loop/thread.
  size_t drain_events(FrameEvent *out, size_t max_events);

  const Relay::Stats &stats() const { return this->relay_.stats(); }
  uint32_t queue_overflows() const { return this->queue_overflows_; }
  const FrameAssembler::Stats &hmi_stats() const { return this->relay_.hmi_stats(); }
  const FrameAssembler::Stats &main_stats() const { return this->relay_.main_stats(); }
  void reset_latency_stats() { this->relay_.reset_latency_stats(); }

  // High-water mark of unused stack, in bytes, for the diagnostic in plan 3.6.3
  // ("measure with uxTaskGetStackHighWaterMark"). 0 before begin() spawns the task.
  uint32_t stack_high_water_mark() const {
    return this->handle_ != nullptr ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(this->handle_)) * sizeof(StackType_t)
                                     : 0;
  }

  // Result of the startup self-test (3.5.4), for a diagnostic text sensor and a
  // one-time log line; never gates whether the relay task itself starts.
  const char *self_test_result() const { return this->self_test_result_; }

 private:
  static void task_entry(void *arg);
  void run();
  static void frame_sink_trampoline(void *ctx, Channel channel, const Frame &f, uint32_t t_us);
  void push_event(Channel channel, const Frame &f, uint32_t t_us);
  void run_self_test();

  UartBusIo hmi_io_;
  UartBusIo main_io_;
  Relay relay_;
  RelayPolicy &policy_;
  int relay_core_;
  bool self_test_enabled_;

  QueueHandle_t events_ = nullptr;
  QueueSetHandle_t queue_set_ = nullptr;
  TaskHandle_t handle_ = nullptr;
  uint32_t queue_overflows_ = 0;
  char self_test_result_[96] = "not run";
};

}  // namespace atlantic_v5

#endif  // USE_ESP32
