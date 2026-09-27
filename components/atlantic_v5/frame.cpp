#include "frame.h"

#include <cstring>

#include "crc16.h"

namespace atlantic_v5 {

namespace {
// Payload-less frame total length: 5-byte header + 2-byte CRC.
constexpr uint8_t PAYLOAD_LESS_LEN = HEADER_LEN + 2;
// Payload starts right after the header and the 1-byte length field.
constexpr uint8_t PAYLOAD_OFFSET = HEADER_LEN + 1;
}  // namespace

Frame::Frame(Channel ch, const uint8_t *data, uint8_t len) : ch_(ch) {
  len_ = len < MAX_FRAME ? len : MAX_FRAME;
  std::memcpy(buf_, data, len_);
}

uint64_t Frame::header_key() const {
  uint64_t key = 0;
  for (size_t i = 0; i < HEADER_LEN; i++)
    key = (key << 8) | buf_[i];
  return key;
}

bool Frame::has_payload() const { return len_ > PAYLOAD_LESS_LEN; }

uint8_t Frame::payload_len() const { return has_payload() ? buf_[HEADER_LEN] : 0; }

uint8_t Frame::buffered_payload_len() const {
  return has_payload() ? static_cast<uint8_t>(len_ - (PAYLOAD_OFFSET + 2)) : 0;
}

const uint8_t *Frame::payload() const { return buf_ + PAYLOAD_OFFSET; }

bool Frame::crc_valid() const {
  if (len_ < 2)
    return false;
  uint16_t computed = crc16_modbus(buf_, len_ - 2);
  uint16_t stored = static_cast<uint16_t>(buf_[len_ - 2]) | (static_cast<uint16_t>(buf_[len_ - 1]) << 8);
  return computed == stored;
}

void Frame::replace_payload(const uint8_t *src) {
  if (len_ < 2)
    return;
  // payload_len() is a wire byte and can claim more than buf_ holds; writing
  // that many bytes at PAYLOAD_OFFSET would run off the end of the frame.
  uint8_t declared = payload_len();
  uint8_t buffered = buffered_payload_len();
  uint8_t n = declared < buffered ? declared : buffered;
  if (n > 0)
    std::memcpy(buf_ + PAYLOAD_OFFSET, src, n);
  uint16_t crc = crc16_modbus(buf_, len_ - 2);
  buf_[len_ - 2] = static_cast<uint8_t>(crc & 0xFF);
  buf_[len_ - 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
  modified_ = true;
}

}  // namespace atlantic_v5
