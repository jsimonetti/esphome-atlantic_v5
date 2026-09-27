#include "relay_task.h"

#ifdef USE_ESP32

#include <cstring>

#include "esphome/core/log.h"

namespace atlantic_v5 {

namespace {
constexpr size_t EVENTS_QUEUE_LEN = 24;      // plan 3.6.5
constexpr uint32_t LISTEN_WINDOW_US = 3'000'000;  // plan 3.5.4 step 1: "wait 3s"
constexpr TickType_t RELAY_BLOCK_TICKS = pdMS_TO_TICKS(2);  // plan 3.6.1
}  // namespace

static const char *const TAG = "atlantic_v5.relay_task";

RelayTask::RelayTask(const Config &cfg, RelayPolicy &policy)
    : hmi_io_(cfg.hmi),
      main_io_(cfg.main),
      relay_(hmi_io_, main_io_, policy, cfg.relay),
      policy_(policy),
      relay_core_(cfg.relay_core),
      self_test_enabled_(cfg.self_test) {
  this->relay_.set_frame_sink(&RelayTask::frame_sink_trampoline, this);
}

void RelayTask::begin() {
  this->hmi_io_.install();
  this->main_io_.install();

  this->events_ = xQueueCreate(EVENTS_QUEUE_LEN, sizeof(FrameEvent));

  this->run_self_test();  // never gates task startup, plan 3.5.4 step 3

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
  // Plan 3.6.1/3.6.4: always block (xQueueSelectFromSet with a bounded timeout),
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
    // signal, never a source of data (plan 3.6.1 #1-2).
    this->relay_.poll(this->hmi_io_.now_us());
  }
}

void RelayTask::frame_sink_trampoline(void *ctx, Channel channel, const Frame &f, uint32_t t_us) {
  static_cast<RelayTask *>(ctx)->push_event(channel, f, t_us);
}

void RelayTask::push_event(Channel channel, const Frame &f, uint32_t t_us) {
  FrameEvent ev{};
  size_t len = f.raw_len() > sizeof(ev.data) ? sizeof(ev.data) : f.raw_len();
  memcpy(ev.data, f.raw(), len);
  ev.len = static_cast<uint8_t>(len);
  ev.channel = channel;
  ev.modified = f.modified();
  ev.crc_ok = f.crc_valid();
  ev.t_us = t_us;

  // Plan 3.6.5: on failure, count and drop — forwarding has already happened by
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

void RelayTask::run_self_test() {
  // Step 1 (plan 3.5.4): both sides already parked in RX by install(); confirm
  // something is arriving on at least one side before deciding anything further.
  uint32_t start_us = this->hmi_io_.now_us();
  bool hmi_seen = false, main_seen = false;
  FrameAssembler hmi_asm(Channel::HMI, /*dual_bus=*/true);
  FrameAssembler main_asm(Channel::MAIN, /*dual_bus=*/true);
  uint8_t hmi_probe[MAX_FRAME]{};
  uint8_t hmi_probe_len = 0;
  uint8_t main_probe[MAX_FRAME]{};
  uint8_t main_probe_len = 0;

  while (this->hmi_io_.now_us() - start_us < LISTEN_WINDOW_US) {
    uint8_t buf[MAX_FRAME];
    uint32_t now_us = this->hmi_io_.now_us();
    int n = this->hmi_io_.read(buf, sizeof(buf), 10000);
    if (n > 0) {
      hmi_seen = true;
      for (int i = 0; i < n; i++) {
        // Remember the first payload-less (harmless to repeat) completed frame.
        if (hmi_asm.push(buf[i], now_us) && hmi_probe_len == 0 && hmi_asm.frame_len() == HEADER_LEN + 2) {
          memcpy(hmi_probe, hmi_asm.frame(), hmi_asm.frame_len());
          hmi_probe_len = hmi_asm.frame_len();
        }
      }
    }
    n = this->main_io_.read(buf, sizeof(buf), 0);
    if (n > 0) {
      main_seen = true;
      for (int i = 0; i < n; i++) {
        if (main_asm.push(buf[i], now_us) && main_probe_len == 0 && main_asm.frame_len() == HEADER_LEN + 2) {
          memcpy(main_probe, main_asm.frame(), main_asm.frame_len());
          main_probe_len = main_asm.frame_len();
        }
      }
    }
  }

  if (!hmi_seen && !main_seen) {
    snprintf(this->self_test_result_, sizeof(this->self_test_result_), "no traffic on either side (wiring/baud?)");
    ESP_LOGE(TAG, "%s", this->self_test_result_);
    return;
  }

  if (!this->self_test_enabled_) {
    snprintf(this->self_test_result_, sizeof(this->self_test_result_), "traffic seen, probe skipped (self_test: false)");
    ESP_LOGI(TAG, "%s", this->self_test_result_);
    return;
  }

  // Step 2: transmit a repeat of an already-seen payload-less header and check
  // whether it echoes back on that same side (plan 3.5.4 step 2). Never forwarded
  // through RelayPolicy/the opposite side — this is a probe, not relayed traffic.
  bool hmi_can_drive = false, main_can_drive = false;
  if (hmi_probe_len > 0) {
    this->hmi_io_.flush_input();
    this->hmi_io_.write(hmi_probe, hmi_probe_len);
    uint8_t buf[MAX_FRAME];
    int n = this->hmi_io_.read(buf, sizeof(buf), 5000);
    hmi_can_drive = n > 0;
  }
  if (main_probe_len > 0) {
    this->main_io_.flush_input();
    this->main_io_.write(main_probe, main_probe_len);
    uint8_t buf[MAX_FRAME];
    int n = this->main_io_.read(buf, sizeof(buf), 5000);
    main_can_drive = n > 0;
  }

  snprintf(this->self_test_result_, sizeof(this->self_test_result_),
           "hmi: %s drive, %s traffic; main: %s drive, %s traffic",
           hmi_can_drive ? "can" : (hmi_probe_len > 0 ? "cannot" : "not probed"), hmi_seen ? "seen" : "no",
           main_can_drive ? "can" : (main_probe_len > 0 ? "cannot" : "not probed"), main_seen ? "seen" : "no");
  ESP_LOGI(TAG, "self-test: %s", this->self_test_result_);
}

}  // namespace atlantic_v5

#endif  // USE_ESP32
