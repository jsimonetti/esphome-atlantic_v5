#include "assembler.h"

#include "frame.h"

namespace atlantic_v5 {

FrameAssembler::FrameAssembler(Channel ch, bool dual_bus, uint32_t silence_us)
    : ch_(ch), dual_bus_(dual_bus), silence_us_(silence_us) {}

// Direction rule from docs/protocol.md "Transaction model": a 7-byte frame is
// payload-less iff the channel and byte[1] (READ/WRITE) match the request/ack side of a
// transaction. Only meaningful
// for dual_bus_; single-bus framing never calls this (no channel to disambiguate with).
bool FrameAssembler::is_payload_less_complete() const {
  if (ch_ == Channel::HMI)
    return buf_[1] == 0x64;
  if (ch_ == Channel::MAIN)
    return buf_[1] == 0x65;
  return false;
}

// Reuses Frame's CRC logic rather than recomputing it, so there is one place
// that knows the CRC layout.
bool FrameAssembler::crc_check(uint8_t len) const {
  Frame f(ch_, buf_, len);
  return f.crc_valid();
}

bool FrameAssembler::finalize(bool crc_ok) {
  if (!crc_ok)
    stats_.crc_errors++;
  stats_.frames++;
  have_frame_ = true;
  return true;
}

void FrameAssembler::reset() {
  len_ = 0;
  have_frame_ = false;
}

bool FrameAssembler::push(uint8_t byte, uint32_t t_us) {
  if (have_frame_) {
    len_ = 0;
    have_frame_ = false;
  }
  last_byte_us_ = t_us;

  // Byte 0 must be the start marker; anything else is noise.
  if (len_ == 0) {
    if (byte != 0x01) {
      stats_.dropped_bytes++;
      stats_.resyncs++;
      return false;
    }
    buf_[len_++] = byte;
    return false;
  }

  // Byte 1 must select READ or WRITE, else byte 0 was noise too.
  if (len_ == 1) {
    if (byte != 0x64 && byte != 0x65) {
      stats_.dropped_bytes += 2;
      stats_.resyncs++;
      len_ = 0;
      return false;
    }
    buf_[len_++] = byte;
    return false;
  }

  buf_[len_++] = byte;
  if (len_ < 7)
    return false;

  if (len_ == 7) {
    if (dual_bus_) {
      // Channel/direction rule (2.2) fully determines payload-less-ness here.
      if (is_payload_less_complete())
        return finalize(crc_check(7));
      // Else this is a payload frame in progress; fall through to the length check below.
    } else {
      // Speculative CRC (2.5.2 #2): a match here is inherently ambiguous, so count it.
      if (crc_check(7)) {
        stats_.speculative_accepts++;
        return finalize(true);
      }
      // No match: could still be a payload frame in progress; fall through.
    }
  }

  // From here buf_[HEADER_LEN] (the length byte) is known; this is a payload frame.
  size_t expected = static_cast<size_t>(HEADER_LEN) + 1 + buf_[HEADER_LEN] + 2;
  if (expected > MAX_FRAME) {
    stats_.oversize++;
    stats_.resyncs++;
    stats_.dropped_bytes += len_;
    len_ = 0;
    return false;
  }
  if (len_ < expected)
    return false;

  bool ok = crc_check(len_);
  if (dual_bus_ || ok)
    return finalize(ok);

  // Single-bus, CRC failed: no direction info to fall back on, drop and resync (2.5.2 #4).
  stats_.crc_errors++;
  stats_.resyncs++;
  stats_.dropped_bytes += len_;
  len_ = 0;
  return false;
}

bool FrameAssembler::tick(uint32_t t_us) {
  if (have_frame_) {
    len_ = 0;
    have_frame_ = false;
  }
  if (len_ == 0)
    return false;
  if (t_us - last_byte_us_ < silence_us_)
    return false;

  // Backstop: close whatever is buffered regardless of length rules.
  stats_.silence_closes++;
  bool ok = crc_check(len_);
  if (dual_bus_ || ok)
    return finalize(ok);

  stats_.crc_errors++;
  stats_.resyncs++;
  stats_.dropped_bytes += len_;
  len_ = 0;
  return false;
}

}  // namespace atlantic_v5
