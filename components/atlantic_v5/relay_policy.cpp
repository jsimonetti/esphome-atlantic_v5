#include "relay_policy.h"

#include <cstring>

namespace atlantic_v5 {

bool RelayPolicy::apply(Frame &f) const {
  switch (f.header_key()) {
    case INPUT_STATUS_HEADER_KEY:
      return apply_input_status(f);
    case TEMPERATURES_HEADER_KEY:
      return apply_temperatures(f);
    default:
      return false;
  }
}

bool RelayPolicy::apply_temperatures(Frame &f) const {
  const int16_t override_centi_c = temperature_override();
  if (override_centi_c == 0)
    return false;
  if (f.payload_len() != TEMPERATURES_PAYLOAD_LEN || f.buffered_payload_len() < TEMPERATURES_PAYLOAD_LEN)
    return false;
  if (!f.crc_valid())
    return false;

  uint8_t new_payload[TEMPERATURES_PAYLOAD_LEN];
  std::memcpy(new_payload, f.payload(), sizeof(new_payload));
  const uint16_t wire = static_cast<uint16_t>(override_centi_c);
  new_payload[0] = static_cast<uint8_t>(wire >> 8);
  new_payload[1] = static_cast<uint8_t>(wire & 0xFF);
  f.replace_payload(new_payload);
  return true;
}

bool RelayPolicy::apply_input_status(Frame &f) const {
  // Declared *and* present: payload()[2] is read below, and a frame closed
  // early by the silence backstop can claim bytes it never carried.
  if (f.payload_len() != INPUT_STATUS_PAYLOAD_LEN || f.buffered_payload_len() < INPUT_STATUS_PAYLOAD_LEN)
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
