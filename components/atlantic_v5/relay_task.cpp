#include "relay_task.h"

#ifdef USE_ESP32

#include <cstring>

namespace atlantic_v5 {

namespace {
constexpr size_t EVENTS_QUEUE_LEN = 24;
constexpr TickType_t RELAY_BLOCK_TICKS = pdMS_TO_TICKS(2);
}  // namespace

RelayTask::RelayTask(const Config &cfg, RelayPolicy &policy)
    : hmi_io_(cfg.hmi),
      main_io_(cfg.main),
      relay_(hmi_io_, main_io_, policy, cfg.relay),
      policy_(policy),
      relay_core_(cfg.relay_core) {
  this->relay_.set_frame_sink(&RelayTask::frame_sink_trampoline, this);
}

void RelayTask::begin() {
  this->hmi_io_.install();
  this->main_io_.install();

  this->events_ = xQueueCreate(EVENTS_QUEUE_LEN, sizeof(FrameEvent));

  // Set size must be >= the sum of both member queues' lengths (16 each, set in
  // UartBusIo::install()'s uart_driver_install call).
  this->queue_set_ = xQueueCreateSet(32);
  xQueueAddToSet(this->hmi_io_.event_queue(), this->queue_set_);
  xQueueAddToSet(this->main_io_.event_queue(), this->queue_set_);

  BaseType_t core = this->relay_core_ < 0 ? tskNO_AFFINITY : this->relay_core_;
  xTaskCreatePinnedToCore(&RelayTask::task_entry, "atlantic_v5_relay",
                          /*stack*/ 4096, this, /*priority*/ 10, &this->handle_, core);
}

void RelayTask::task_entry(void *arg) {
  static_cast<RelayTask *>(arg)->run();
}

void RelayTask::run() {
  // Always block (xQueueSelectFromSet with a bounded timeout),
  // never a bare uart_get_buffered_data_len() spin, and never subscribed to the
  // task watchdog (a hang here means a dead bus, which the peers report
  // themselves, not a reason to reboot mid-appliance-bus).
  while (true) {
    QueueSetMemberHandle_t activated = xQueueSelectFromSet(this->queue_set_, RELAY_BLOCK_TICKS);
    if (activated != nullptr) {
      uart_event_t event;
      xQueueReceive(static_cast<QueueHandle_t>(activated), &event, 0);
    }
    // relay_.poll() does its own uart_read_bytes/write regardless of which port's
    // event woke us (or none, on a plain timeout) — the event is only a wakeup
    // signal, never a source of data.
    this->relay_.poll(this->hmi_io_.now_us());
  }
}

void RelayTask::frame_sink_trampoline(void *ctx, Channel channel, const Frame &f, const uint8_t *observed_payload,
                                      uint32_t t_us) {
  static_cast<RelayTask *>(ctx)->push_event(channel, f, observed_payload, t_us);
}

void RelayTask::push_event(Channel channel, const Frame &f, const uint8_t *observed_payload, uint32_t t_us) {
  FrameEvent ev{};
  size_t len = f.raw_len() > sizeof(ev.data) ? sizeof(ev.data) : f.raw_len();
  memcpy(ev.data, f.raw(), len);
  ev.len = static_cast<uint8_t>(len);
  ev.channel = channel;
  ev.modified = f.modified();
  ev.crc_ok = f.crc_valid();
  ev.t_us = t_us;
  if (observed_payload != nullptr)
    memcpy(ev.observed_payload, observed_payload, REWRITE_PAYLOAD_LEN);

  // On failure, count and drop — forwarding has already happened by
  // this point (this runs from Relay's frame sink, called after the write), so a
  // dropped event never affects forwarding.
  if (xQueueSend(this->events_, &ev, 0) != pdTRUE)
    this->queue_overflows_++;
}

size_t RelayTask::drain_events(FrameEvent *out, size_t max_events) {
  size_t n = 0;
  while (n < max_events && xQueueReceive(this->events_, &out[n], 0) == pdTRUE)
    n++;
  return n;
}

}  // namespace atlantic_v5

#endif  // USE_ESP32
