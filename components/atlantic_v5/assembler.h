// L1 core. Host-compilable: no esp-idf, no ESPHome, no Arduino, no exceptions, no heap.
#pragma once

#include "types.h"

namespace atlantic_v5 {

// Byte-at-a-time frame assembly per docs/protocol.md "Framing rules". Two modes
// selected at construction:
// dual_bus=true disambiguates payload-less frames by channel/direction ("Dual-bus") and
// always delivers whatever it buffers once length rules close it, even on CRC
// failure (fail-safe passthrough; the caller forwards raw bytes regardless).
// dual_bus=false is the single-wire listener path ("Single-bus"): it speculatively
// CRC-checks at 7 bytes and drops+resyncs the whole buffer on any CRC failure,
// since there is no channel/direction information to fall back on.
class FrameAssembler {
 public:
  static constexpr uint32_t DEFAULT_SILENCE_US = 4000;  // framing backstop threshold

  // silence_us is injectable so tests can vary it; the transport layer wires in its
  // own configured value.
  explicit FrameAssembler(Channel ch, bool dual_bus, uint32_t silence_us = DEFAULT_SILENCE_US);

  // Returns true when frame()/frame_len() hold a complete candidate frame.
  // The next push() call invalidates it, so the caller must consume it first.
  bool push(uint8_t byte, uint32_t t_us);

  // Call periodically even when no bytes arrive, to apply the silence backstop.
  // Same consume-before-next-call contract as push().
  bool tick(uint32_t t_us);

  const uint8_t *frame() const { return buf_; }
  uint8_t frame_len() const { return len_; }

  // Discards any partially-buffered frame without touching stats().
  void reset();

  // For callers that can't pass the value at construction because the
  // assembler is a member of a default-constructed owner (Listener).
  void set_silence_us(uint32_t us) { silence_us_ = us; }

  struct Stats {
    uint32_t frames = 0;
    uint32_t crc_errors = 0;
    uint32_t resyncs = 0;
    uint32_t dropped_bytes = 0;
    uint32_t oversize = 0;
    uint32_t speculative_accepts = 0;
    uint32_t silence_closes = 0;
  };
  const Stats &stats() const { return stats_; }

 private:
  bool is_payload_less_complete() const;
  bool crc_check(uint8_t len) const;
  bool finalize(bool crc_ok);

  Channel ch_;
  bool dual_bus_;
  uint32_t silence_us_;
  uint8_t buf_[MAX_FRAME];
  uint8_t len_ = 0;
  uint32_t last_byte_us_ = 0;
  bool have_frame_ = false;
  Stats stats_;
};

}  // namespace atlantic_v5
