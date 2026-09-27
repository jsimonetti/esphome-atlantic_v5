#include "relay.h"

#include <cstring>

#include "frame.h"

namespace atlantic_v5 {

Relay::Relay(BusIo &hmi_io, BusIo &main_io, RelayPolicy &policy, Config cfg)
    : hmi_{hmi_io, FrameAssembler(Channel::HMI, /*dual_bus=*/true, cfg.silence_us)},
      main_{main_io, FrameAssembler(Channel::MAIN, /*dual_bus=*/true, cfg.silence_us)},
      policy_(policy),
      cfg_(cfg) {}

void Relay::poll(uint32_t now_us) {
  service(hmi_, main_, Channel::HMI, now_us);
  service(main_, hmi_, Channel::MAIN, now_us);
}

void Relay::service(Side &in, Side &out, Channel in_channel, uint32_t now_us) {
  uint8_t buf[MAX_FRAME];
  int n = in.io.read(buf, sizeof(buf), 0);
  if (n > 0) {
    if (capture_sink_ != nullptr)
      capture_sink_(capture_ctx_, in_channel, buf, static_cast<size_t>(n), now_us);
    if (now_us < in.echo_until_us) {
      // Our own echo of a write to this side (3.5.3): discard, never frame it.
      stats_.echo_bytes += static_cast<uint32_t>(n);
    } else {
      for (int i = 0; i < n; i++) {
        if (in.asm_.push(buf[i], now_us))
          forward(in, out, in_channel, now_us);
      }
    }
  }

  // Silence backstop (2.5.1 #6), independent of whether bytes just arrived.
  if (in.asm_.tick(now_us))
    forward(in, out, in_channel, now_us);
}

void Relay::forward(Side &in, Side &out, Channel in_channel, uint32_t now_us) {
  Frame f(in_channel, in.asm_.frame(), in.asm_.frame_len());
  // Snapshot the payload before the rewrite: the wire gets the rewritten frame,
  // the decoder gets what the appliance actually reported (ADR 0002).
  uint8_t observed[REWRITE_PAYLOAD_LEN] = {};
  if (f.payload_len() == REWRITE_PAYLOAD_LEN && f.buffered_payload_len() >= REWRITE_PAYLOAD_LEN)
    std::memcpy(observed, f.payload(), REWRITE_PAYLOAD_LEN);

  // apply() is itself a no-op on a bad-CRC or non-matching frame, which gives us
  // the fail-safe forwarding rule (plan 2.5.1 / 3.9 #2) for free: forward first,
  // exactly as received unless the rewrite hook says otherwise.
  bool rewritten = policy_.apply(f);
  out.io.write(f.raw(), f.raw_len());
  out.io.flush_input();
  out.echo_until_us = now_us + cfg_.echo_drain_us;

  stats_.frames_relayed++;
  if (rewritten)
    stats_.rewrites_applied++;

  if (frame_sink_ != nullptr)
    frame_sink_(frame_ctx_, in_channel, f, rewritten ? observed : nullptr, now_us);

  // Latency here is last-byte-in (now_us, the timestamp the completed frame was
  // detected at) to write-initiated (plan 3.6.2, excluding fixed transmit time).
  // out.io.now_us() is read immediately after the write, so in the synchronous
  // host model (poll() called once per row with a single timestamp for both
  // sides, MockBusIo's clock never advancing mid-call) this is still exactly 0,
  // preserving every existing host assertion — but on real hardware (M6's
  // RelayTask, a real clock) this now reports the actual wakeup+framing+policy
  // overhead the relay task spent before this write, evidence plan 3.6.2 asks
  // for instead of a permanently-stubbed 0.
  uint32_t latency_us = out.io.now_us() - now_us;
  stats_.latency_total_us += latency_us;
  stats_.latency_samples++;
  if (latency_us > stats_.latency_max_us)
    stats_.latency_max_us = latency_us;
}

}  // namespace atlantic_v5
