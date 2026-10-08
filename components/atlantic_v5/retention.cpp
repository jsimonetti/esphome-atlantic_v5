#include "retention.h"

#include <cstring>

#include "crc16.h"

namespace atlantic_v5 {

namespace {
// 'A','V','5','R'. Any constant would do; this one is at least recognisable in
// a memory dump.
constexpr uint32_t MAGIC = 0x41563552u;
}  // namespace

// The CRC covers the layout version and the payload length as well as the
// payload itself, so every byte load() bases a decision on is checksummed
// rather than just the bytes it hands back.
uint16_t RetentionBlock::covered_crc(uint16_t len) const {
  return crc16_modbus(reinterpret_cast<const uint8_t *>(&this->layout_version_),
                      sizeof(this->layout_version_) + sizeof(this->payload_len_) + len);
}

bool RetentionBlock::store(const uint8_t *data, uint16_t len) {
  if (len > CAPACITY) {
    this->magic_ = 0;
    return false;
  }
  this->layout_version_ = LAYOUT_VERSION;
  this->payload_len_ = len;
  if (len > 0)
    std::memcpy(this->payload_, data, len);
  this->crc_ = this->covered_crc(len);
  // Written last, so a store interrupted by a reset leaves a block that reads
  // as absent rather than as a value with a stale payload.
  this->magic_ = MAGIC;
  return true;
}

bool RetentionBlock::load(uint8_t *out, uint16_t cap, uint16_t &len_out) const {
  if (this->magic_ != MAGIC)
    return false;
  if (this->layout_version_ != LAYOUT_VERSION)
    return false;
  const uint16_t len = this->payload_len_;
  if (len > CAPACITY)
    return false;
  if (this->crc_ != this->covered_crc(len))
    return false;
  if (len > cap)
    return false;
  if (len > 0)
    std::memcpy(out, this->payload_, len);
  len_out = len;
  return true;
}

}  // namespace atlantic_v5
