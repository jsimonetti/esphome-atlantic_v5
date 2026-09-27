#include "relay_policy.h"

namespace atlantic_v5 {

bool RelayPolicy::apply(Frame &f) const {
  if (f.header_key() != INPUT_STATUS_HEADER_KEY)
    return false;
  if (f.payload_len() != INPUT_STATUS_PAYLOAD_LEN)
    return false;
  if (!f.crc_valid())
    return false;

  uint8_t i2, i1;
  switch (control_mode()) {
    case ControlMode::NORMAL:
      i2 = 0;
      i1 = 0;
      break;
    case ControlMode::EAGER:
      i2 = 0;
      i1 = 1;
      break;
    case ControlMode::OFF:
      i2 = 1;
      i1 = 0;
      break;
    case ControlMode::BOOST:
      i2 = 1;
      i1 = 1;
      break;
    case ControlMode::PASSTHROUGH:
    default:
      return false;
  }

  const uint8_t new_payload[INPUT_STATUS_PAYLOAD_LEN] = {i2, i1, f.payload()[2]};
  f.replace_payload(new_payload);
  return true;
}

}  // namespace atlantic_v5
