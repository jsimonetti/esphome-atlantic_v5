// L2 transport, hardware-free listener-mode glue (plan M4 / 3.7.1). Wraps the
// single tapped-bus FrameAssembler (2.5.2 speculative-CRC framing) and the
// Decoder, and tracks the MAIN-quiet staleness gate independently of HMI
// traffic. Verifiable on a host with no ESPHome headers: L3 feeds it bytes
// from the UART and a sink from a publish callback; tests feed it bytes
// straight from a capture.
#pragma once

#include "assembler.h"
#include "decoder.h"
#include "frame.h"
#include "types.h"

namespace atlantic_v5 {

class Listener {
 public:
  using Sink = Decoder::Sink;

  void set_sink(Sink sink, void *ctx) {
    sink_ = sink;
    ctx_ = ctx;
  }

  // Raw already-framed, CRC-valid frame hook (plan 3.8's raw_frame_dump),
  // fired for every complete frame regardless of whether Decoder recognises
  // its header - mirrors Relay::FrameSink (ticket 08) so both modes share the
  // same "who gets to see a completed frame" shape.
  using FrameSink = void (*)(void *ctx, const Frame &f, uint32_t t_us);
  void set_frame_sink(FrameSink sink, void *ctx) {
    frame_sink_ = sink;
    frame_ctx_ = ctx;
  }

  // Feeds one byte from the tapped bus. Decodes and publishes through the sink
  // whenever a complete, CRC-valid frame closes; a bad CRC is fail-safe
  // discarded (plan 2.5.2 #4 already resyncs the assembler on this path).
  void push_byte(uint8_t byte, uint32_t t_us);

  // Call every loop tick, even with no new bytes: applies the assembler's
  // silence backstop (2.1/2.5.1) so a partially-buffered frame doesn't wedge
  // the staleness gate open forever.
  void tick(uint32_t t_us);

  // Microseconds since the last CRC-valid, payload-bearing MAIN-origin frame
  // (plan 2.2: a payload-bearing 0x64 frame can only be MAIN's response - HMI's
  // 0x64 requests are always payload-less). Callers compare this against their
  // own configured timeout; HMI-only traffic never resets it (ticket 07).
  uint32_t us_since_main(uint32_t t_us) const { return t_us - last_main_us_; }

  // Whether any such frame has been seen at all. us_since_main() can't say:
  // before the first frame it just counts up from t=0, which is
  // indistinguishable from a frame that genuinely arrived at t=0.
  bool has_main() const { return has_main_; }

  const FrameAssembler::Stats &assembler_stats() const { return assembler_.stats(); }
  const Decoder::Stats &decoder_stats() const { return decoder_.stats(); }

 private:
  void handle_frame(uint32_t t_us);

  FrameAssembler assembler_{Channel::BUS, false};
  Decoder decoder_;
  uint32_t last_main_us_ = 0;
  bool has_main_ = false;
  Sink sink_ = nullptr;
  void *ctx_ = nullptr;
  FrameSink frame_sink_ = nullptr;
  void *frame_ctx_ = nullptr;
};

}  // namespace atlantic_v5
