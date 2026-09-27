// L2 transport, hardware-free (plan M5 / 3.6.1 minus the FreeRTOS/queue specifics,
// which are RelayTask's job at M6). Driven purely by BusIo + a caller-owned clock,
// so it is fully verifiable on a host with a mock BusIo and no Decoder dependency.
#pragma once

#include "bus_io.h"
#include "assembler.h"
#include "frame.h"
#include "relay_policy.h"
#include "types.h"

namespace atlantic_v5 {

// Declared outside Relay: a nested struct's default member initializers can't be
// used in a default argument of the enclosing class's own constructor.
struct RelayConfig {
  uint32_t silence_us = FrameAssembler::DEFAULT_SILENCE_US;  // plan 2.1/2.5.1 backstop
  uint32_t echo_drain_us = 200;                              // plan 3.5.3 default
};

class Relay {
 public:
  using Config = RelayConfig;

  Relay(BusIo &hmi_io, BusIo &main_io, RelayPolicy &policy, Config cfg = Config{});

  // Raw pre-assembly byte capture (plan 3.5.5, MITM piggyback): invoked with every
  // chunk read from either side, before echo classification, as a byproduct of
  // reads the relay task is already doing — never an extra poll. Set once, before
  // the first poll() call.
  using CaptureSink = void (*)(void *ctx, Channel channel, const uint8_t *data, size_t len, uint32_t t_us);
  void set_capture_sink(CaptureSink sink, void *ctx) {
    capture_sink_ = sink;
    capture_ctx_ = ctx;
  }

  // Cross-thread handoff (plan 3.6.5): invoked once per completed frame, after it
  // has already been forwarded to the opposite side ("forward first, enqueue
  // second", plan 3.9 #1) — a sink that drops the event can never affect
  // forwarding, which has already happened by the time this runs. f reflects any
  // rewrite RelayPolicy::apply already applied, i.e. the bytes that went out on
  // the wire. observed_payload is the payload as received, non-null only when a
  // rewrite was applied; decode that rather than f's payload (ADR 0002).
  using FrameSink = void (*)(void *ctx, Channel channel, const Frame &f, const uint8_t *observed_payload,
                             uint32_t t_us);
  void set_frame_sink(FrameSink sink, void *ctx) {
    frame_sink_ = sink;
    frame_ctx_ = ctx;
  }

  // Services one iteration for both directions at now_us: drains whatever bytes
  // are currently available on each side, applies the silence backstop, and
  // forwards any completed frame to the opposite side (running it through the
  // rewrite hook first). Bytes arriving on a side within echo_drain_us of a write
  // to that same side are treated as our own echo: discarded and counted, never
  // fed to that side's assembler. Call repeatedly with a monotonically
  // non-decreasing now_us (mock clock in host tests, real clock in the relay task).
  void poll(uint32_t now_us);

  struct Stats {
    uint32_t frames_relayed = 0;
    uint32_t rewrites_applied = 0;
    uint32_t echo_bytes = 0;
    uint32_t latency_max_us = 0;
    uint64_t latency_total_us = 0;
    uint32_t latency_samples = 0;
  };
  const Stats &stats() const { return stats_; }

  // plan 3.8: relay_latency_avg_us/max_us are "reset on read or on an hourly
  // window" - the diagnostics publish cycle calls this right after reading, so
  // each published value covers "since the last read", not "since boot".
  // frames_relayed/rewrites_applied/echo_bytes are plain cumulative counters
  // and are never reset.
  void reset_latency_stats() {
    stats_.latency_max_us = 0;
    stats_.latency_total_us = 0;
    stats_.latency_samples = 0;
  }
  const FrameAssembler::Stats &hmi_stats() const { return hmi_.asm_.stats(); }
  const FrameAssembler::Stats &main_stats() const { return main_.asm_.stats(); }

 private:
  struct Side {
    BusIo &io;
    FrameAssembler asm_;
    uint32_t echo_until_us = 0;  // reads before this time are our own echo, see 3.5.3
  };

  // Drains in.io, feeds in.asm_, and forwards any completed frame to out.io.
  // in_channel identifies which physical side `in` is, for Frame construction.
  void service(Side &in, Side &out, Channel in_channel, uint32_t now_us);
  void forward(Side &in, Side &out, Channel in_channel, uint32_t now_us);

  Side hmi_;
  Side main_;
  RelayPolicy &policy_;
  Config cfg_;
  Stats stats_;
  CaptureSink capture_sink_ = nullptr;
  void *capture_ctx_ = nullptr;
  FrameSink frame_sink_ = nullptr;
  void *frame_ctx_ = nullptr;
};

}  // namespace atlantic_v5
