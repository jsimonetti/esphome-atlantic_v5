// L2 transport, hardware-free (plan M5 / 3.6.1 minus the FreeRTOS/queue specifics,
// which are RelayTask's job at M6). Driven purely by BusIo + a caller-owned clock,
// so it is fully verifiable on a host with a mock BusIo and no Decoder dependency.
#pragma once

#include "bus_io.h"
#include "core/assembler.h"
#include "core/relay_policy.h"
#include "core/types.h"

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
};

}  // namespace atlantic_v5
