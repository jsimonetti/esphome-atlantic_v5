// L1 core. Host-compilable: no esp-idf, no ESPHome, no Arduino, no exceptions, no heap.
#pragma once

#include <cstddef>
#include <cstdint>

namespace atlantic_v5 {

// A payload plus the guard that decides whether it is a payload at all.
//
// The instance this exists for lives in memory the C runtime deliberately does
// not initialise (see retained_ram.h), so on any boot the bytes are either
// something an earlier run of this firmware wrote or whatever the SRAM happens
// to hold. load() tells those two apart; nothing else in the codebase should
// try to.
class RetentionBlock {
 public:
  static constexpr uint16_t CAPACITY = 256;

  // Bump whenever the meaning of the payload bytes changes. A block written by
  // a previous firmware is then rejected outright rather than decoded under the
  // new meaning.
  static constexpr uint16_t LAYOUT_VERSION = 1;

  // Replaces the block's contents with payload[0..len). A len above CAPACITY is
  // refused, and leaves the block in a state load() rejects rather than a
  // half-written one.
  bool store(const uint8_t *data, uint16_t len);

  // Copies the retained payload into out[0..cap) and reports its length.
  // Returns false - writing nothing - unless the magic, the layout version, the
  // length and the CRC all agree, or the payload does not fit in cap.
  bool load(uint8_t *out, uint16_t cap, uint16_t &len_out) const;

 private:
  uint16_t covered_crc(uint16_t len) const;

  uint32_t magic_;
  uint16_t crc_;
  uint16_t layout_version_;
  uint16_t payload_len_;
  uint8_t payload_[CAPACITY];
};

static_assert(sizeof(RetentionBlock) == 12 + RetentionBlock::CAPACITY,
              "RetentionBlock must stay padding-free: covered_crc() reads layout_version_, "
              "payload_len_ and payload_ as one contiguous run of bytes");

}  // namespace atlantic_v5
