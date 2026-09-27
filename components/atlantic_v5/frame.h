// L1 core. Host-compilable: no esp-idf, no ESPHome, no Arduino, no exceptions, no heap.
#pragma once

#include "types.h"

namespace atlantic_v5 {

// Thin, fixed-size view over one complete frame (plan 2.3). Construction from an
// incomplete or otherwise not-yet-framed byte run is the FrameAssembler's job (M2),
// not this class's: Frame trusts len to already delimit one candidate frame.
class Frame {
 public:
  Frame(Channel ch, const uint8_t *data, uint8_t len);

  Channel channel() const { return ch_; }

  // The 5 header bytes as a single big-endian key, per plan 2.3 (dispatch key).
  uint64_t header_key() const;

  // &raw()[HEADER_LEN + 1]. Only meaningful when has_payload().
  const uint8_t *payload() const;
  // 0 for a payload-less (7-byte) frame; otherwise raw()[HEADER_LEN]. This is a
  // raw wire byte: a CRC-valid frame can still claim more than it carries, so
  // anything reading payload() must bound itself by buffered_payload_len().
  uint8_t payload_len() const;
  // How many payload bytes the frame actually holds, whatever payload_len() claims.
  uint8_t buffered_payload_len() const;
  bool has_payload() const;

  // False for anything shorter than a minimal CRC-terminated frame.
  bool crc_valid() const;

  // Replaces the frame's payload bytes and recomputes/rewrites the trailing
  // little-endian CRC. Writes min(payload_len(), buffered_payload_len()) bytes;
  // caller guarantees src holds at least that many.
  void replace_payload(const uint8_t *src);

  bool modified() const { return modified_; }
  const uint8_t *raw() const { return buf_; }
  uint8_t raw_len() const { return len_; }

 private:
  uint8_t buf_[MAX_FRAME];
  uint8_t len_;
  Channel ch_;
  bool modified_{false};
};

}  // namespace atlantic_v5
