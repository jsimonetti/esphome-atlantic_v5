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
#ifdef ATLANTIC_V5_LINK_BLACKOUT
  update_blackout(now_us);
#endif
  service(hmi_, main_, Channel::HMI, now_us);
  service(main_, hmi_, Channel::MAIN, now_us);
}

#ifdef ATLANTIC_V5_LINK_BLACKOUT
void Relay::update_blackout(uint32_t now_us) {
  if (!blackout_requested_.load(std::memory_order_relaxed)) {
    blackout_active_ = false;
    return;
  }
  if (!blackout_active_) {
    // The window starts on the relay's own clock, never on the requester's: a
    // latch that depends on the main task, WiFi or the API to clear it can
    // strand a live appliance with its control link severed.
    blackout_start_us_ = now_us;
    blackout_active_ = true;
    return;
  }
  // Elapsed-since rather than a stored deadline, for the same reason as the
  // echo window in service(): now_us wraps every ~71.6 min.
  if (now_us - blackout_start_us_ >= cfg_.blackout_us) {
    blackout_active_ = false;
    blackout_requested_.store(false, std::memory_order_relaxed);
  }
}

bool Relay::blackout_drops(Channel in_channel) const {
  if (!blackout_active_)
    return false;
  switch (cfg_.blackout_direction) {
    case BlackoutDirection::MAIN_TO_HMI:
      return in_channel == Channel::MAIN;
    case BlackoutDirection::HMI_TO_MAIN:
      return in_channel == Channel::HMI;
    case BlackoutDirection::BOTH:
      return true;
  }
  return false;
}
#endif

void Relay::service(Side &in, Side &out, Channel in_channel, uint32_t now_us) {
  uint8_t buf[MAX_FRAME];
  int n = in.io.read(buf, sizeof(buf), 0);
  if (n < 0)
    n = 0;

  // Called on every poll, including empty ones: the sink's own silence backstop
  // can only fire when it is told that no bytes arrived.
  if (capture_sink_ != nullptr)
    capture_sink_(capture_ctx_, in_channel, buf, static_cast<size_t>(n), now_us);

  // Bytes arriving inside the echo window are our own echo of a write to this
  // side (3.5.3): dropped on the floor, never framed. Expressed as elapsed-since-write
  // rather than a stored deadline compared with >=, because now_us is a 32-bit
  // truncation of esp_timer_get_time() and wraps every ~71.6 min: a deadline
  // comparison reads as "still inside the window" for a whole wrap period after
  // the wrap, and since only a forwarded frame refreshes it, the relay never
  // recovers. Unsigned subtraction is correct across the wrap.
  if (n > 0 && now_us - in.last_write_us >= cfg_.echo_drain_us) {
    for (int i = 0; i < n; i++) {
      if (in.asm_.push(buf[i], now_us))
        forward(in, out, in_channel, now_us);
    }
  }

  // Silence backstop (2.5.1 #6), independent of whether bytes just arrived.
  if (in.asm_.tick(now_us))
    forward(in, out, in_channel, now_us);
}

void Relay::forward(Side &in, Side &out, Channel in_channel, uint32_t now_us) {
  Frame f(in_channel, in.asm_.frame(), in.asm_.frame_len());

  // Dropped before the frame sink too: loop_mitm() already refuses to decode a
  // bad-CRC frame, so enqueueing one only costs event-queue space.
  if (!cfg_.forward_bad_crc && !f.crc_valid())
    return;

#ifdef ATLANTIC_V5_LINK_BLACKOUT
  if (blackout_drops(in_channel)) {
    // The sink still runs: the frames that were not forwarded are exactly the
    // evidence a blackout exists to collect. Nothing is rewritten, nothing
    // reaches the wire, and frames_relayed is not touched.
    if (frame_sink_ != nullptr)
      frame_sink_(frame_ctx_, in_channel, f, nullptr, now_us);
    return;
  }
#endif

  // Snapshot the payload before the rewrite: the wire gets the rewritten frame,
  // the decoder gets what the appliance actually reported (ADR 0002).
  uint8_t observed[REWRITE_PAYLOAD_LEN] = {};
  if (f.payload_len() == REWRITE_PAYLOAD_LEN && f.buffered_payload_len() >= REWRITE_PAYLOAD_LEN)
    std::memcpy(observed, f.payload(), REWRITE_PAYLOAD_LEN);

  // apply() is itself a no-op on a bad-CRC or non-matching frame, so a frame that
  // only got this far because forward_bad_crc is set still goes out exactly as
  // received.
  bool rewritten = policy_.apply(f);
  out.io.write(f.raw(), f.raw_len());
  // Sampled after write(), which blocks until the last bit is out: anchoring the
  // echo window on now_us (the last byte *in*) expires it a whole frame-time
  // before the first echoed byte can arrive.
  uint32_t written_us = out.io.now_us();
  out.io.flush_input();
  out.last_write_us = written_us;

  stats_.frames_relayed++;
  if (rewritten)
    stats_.rewrites_applied++;

  if (frame_sink_ != nullptr)
    frame_sink_(frame_ctx_, in_channel, f, rewritten ? observed : nullptr, now_us);

  // Latency here is last-byte-in (now_us, the timestamp the completed frame was
  // detected at) to write-complete, so on real hardware it includes the frame's
  // own transmit time. In the synchronous host model (poll() called once per row
  // with a single timestamp for both sides, MockBusIo's clock never advancing
  // mid-call) it is still exactly 0, preserving every existing host assertion.
  uint32_t latency_us = written_us - now_us;
  stats_.latency_total_us += latency_us;
  stats_.latency_samples++;
  if (latency_us > stats_.latency_max_us)
    stats_.latency_max_us = latency_us;
}

}  // namespace atlantic_v5
