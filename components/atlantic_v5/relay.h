// L2 transport, hardware-free (the FreeRTOS/queue specifics are RelayTask's job).
// Driven purely by BusIo + a caller-owned clock,
// so it is fully verifiable on a host with a mock BusIo and no Decoder dependency.
#pragma once

#include "bus_io.h"
#include "assembler.h"
#include "frame.h"
#include "relay_policy.h"
#include "types.h"

#ifdef ATLANTIC_V5_LINK_BLACKOUT
#include <atomic>
#endif

namespace atlantic_v5 {

inline constexpr uint32_t DEFAULT_ECHO_DRAIN_US = 200;

#ifdef ATLANTIC_V5_LINK_BLACKOUT
// Which forwarding direction a link blackout severs. MAIN_TO_HMI is the
// default: MAIN keeps receiving the HMI's requests and answering them, so it
// never experiences a loss and the disturbance to a live appliance is the
// smallest one that can still provoke the HMI.
enum class BlackoutDirection : uint8_t { MAIN_TO_HMI, HMI_TO_MAIN, BOTH };

inline constexpr uint32_t DEFAULT_BLACKOUT_US = 15'000'000;
#endif

// Declared outside Relay: a nested struct's default member initializers can't be
// used in a default argument of the enclosing class's own constructor.
struct RelayConfig {
  uint32_t silence_us = FrameAssembler::DEFAULT_SILENCE_US;  // framing backstop
  // Not exposed in YAML: flush_input() after each write already discards the
  // echo, so this only ever catches a byte that lands after that flush.
  uint32_t echo_drain_us = DEFAULT_ECHO_DRAIN_US;
  // Escape hatch for a bus whose framing this component gets wrong: relaying a
  // frame the far end will reject on CRC anyway is only useful if the CRC
  // failure is ours, not the sender's.
  bool forward_bad_crc = false;
#ifdef ATLANTIC_V5_LINK_BLACKOUT
  BlackoutDirection blackout_direction = BlackoutDirection::MAIN_TO_HMI;
  uint32_t blackout_us = DEFAULT_BLACKOUT_US;
#endif
};

class Relay {
 public:
  using Config = RelayConfig;

  Relay(BusIo &hmi_io, BusIo &main_io, RelayPolicy &policy, Config cfg = Config{});

  // Raw pre-assembly byte capture (MITM piggyback): invoked with every
  // chunk read from either side, before echo classification, as a byproduct of
  // reads the relay task is already doing — never an extra poll. Set once, before
  // the first poll() call.
  using CaptureSink = void (*)(void *ctx, Channel channel, const uint8_t *data, size_t len, uint32_t t_us);
  void set_capture_sink(CaptureSink sink, void *ctx) {
    capture_sink_ = sink;
    capture_ctx_ = ctx;
  }

  // Cross-thread handoff: invoked once per completed frame, after it
  // has already been forwarded to the opposite side ("forward first, enqueue
  // second") — a sink that drops the event can never affect
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
  // to that same side are treated as our own echo: discarded, never fed to that
  // side's assembler. A completed frame whose CRC fails is dropped
  // rather than forwarded unless cfg.forward_bad_crc is set; either way the
  // assembler has already counted it in crc_errors. Call repeatedly with a monotonically
  // non-decreasing now_us (mock clock in host tests, real clock in the relay task).
  void poll(uint32_t now_us);

  struct Stats {
    uint32_t frames_relayed = 0;
    uint32_t rewrites_applied = 0;
    uint32_t latency_max_us = 0;
    uint64_t latency_total_us = 0;
    uint32_t latency_samples = 0;
  };
  const Stats &stats() const { return stats_; }

  // relay_latency_avg_us/max_us are reset on read: the diagnostics publish cycle
  // calls this right after reading, so
  // each published value covers "since the last read", not "since boot".
  // frames_relayed/rewrites_applied are plain cumulative counters
  // and are never reset.
  void reset_latency_stats() {
    stats_.latency_max_us = 0;
    stats_.latency_total_us = 0;
    stats_.latency_samples = 0;
  }
  const FrameAssembler::Stats &hmi_stats() const { return hmi_.asm_.stats(); }
  const FrameAssembler::Stats &main_stats() const { return main_.asm_.stats(); }

#ifdef ATLANTIC_V5_LINK_BLACKOUT
  // Link blackout: forwarding is severed in cfg.blackout_direction for
  // cfg.blackout_us, so the HMI loses MAIN and replays its initialisation burst
  // when forwarding resumes. Requested and released from any thread; the window
  // itself is latched and expired inside poll(), on the relay's own clock, so
  // nothing outside the relay task can leave the control link cut.
  void request_blackout() { blackout_requested_.store(true, std::memory_order_relaxed); }
  void release_blackout() { blackout_requested_.store(false, std::memory_order_relaxed); }
  // True from the moment a blackout is requested until it is released, by hand
  // or by the relay's own auto-release. Deliberately not "is the cut in force
  // right now": the cut only starts at the next poll(), and an entity mirroring
  // this must not report "off" in that gap.
  bool blackout_engaged() const { return blackout_requested_.load(std::memory_order_relaxed); }
#endif

 private:
  struct Side {
    BusIo &io;
    FrameAssembler asm_;
    // Reads within echo_drain_us of this are our own echo, see 3.5.3.
    uint32_t last_write_us = 0;
  };

  // Drains in.io, feeds in.asm_, and forwards any completed frame to out.io.
  // in_channel identifies which physical side `in` is, for Frame construction.
  void service(Side &in, Side &out, Channel in_channel, uint32_t now_us);
  void forward(Side &in, Side &out, Channel in_channel, uint32_t now_us);

#ifdef ATLANTIC_V5_LINK_BLACKOUT
  void update_blackout(uint32_t now_us);
  bool blackout_drops(Channel in_channel) const;
#endif

  Side hmi_;
  Side main_;
  RelayPolicy &policy_;
  Config cfg_;
  Stats stats_;
#ifdef ATLANTIC_V5_LINK_BLACKOUT
  // Only blackout_requested_ crosses threads; the other two are touched
  // exclusively by the relay task, inside poll().
  std::atomic<bool> blackout_requested_{false};
  bool blackout_active_ = false;
  uint32_t blackout_start_us_ = 0;
#endif
  CaptureSink capture_sink_ = nullptr;
  void *capture_ctx_ = nullptr;
  FrameSink frame_sink_ = nullptr;
  void *frame_ctx_ = nullptr;
};

}  // namespace atlantic_v5
