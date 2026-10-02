#include "decoder.h"

#include <cstring>

#include "catalog.h"

namespace atlantic_v5 {

namespace codec {

float decode_temp(const uint8_t *b) {
  int16_t raw = static_cast<int16_t>((static_cast<uint16_t>(b[0]) << 8) | b[1]);
  return static_cast<float>(raw) / 100.0f;
}

uint32_t decode_u32(const uint8_t *b) {
  return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
         (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
}

uint16_t decode_u16(const uint8_t *b) {
  return static_cast<uint16_t>((static_cast<uint16_t>(b[0]) << 8) | b[1]);
}

bool decode_bool(const uint8_t *b) { return b[0] != 0; }

bool decode_text(const uint8_t *b, uint8_t len, char *out, size_t out_len) {
  if (len == 0 || b[len - 1] != 0x00)
    return false;
  size_t n = 0;
  while (n < len && b[n] != 0x00 && n + 1 < out_len)
    n++;
  std::memcpy(out, b, n);
  out[n] = '\0';
  return true;
}

bool decode_minmax(const uint8_t *b, float *min_c, float *max_c) {
  if (b[0] != 0x00)
    return false;
  *min_c = decode_temp(b + 1);
  *max_c = decode_temp(b + 3);
  return true;
}

Cycle decode_cycle(const uint8_t *b) {
  uint32_t secs1 = decode_u32(b + 4);
  uint32_t count = decode_u32(b + 8);
  return Cycle{secs1 > 0, count};
}

}  // namespace codec

namespace {

void emit_float(Decoder::Sink sink, void *ctx, uint16_t id, float f) {
  DecodedValue v{};
  v.kind = DecodedValue::Kind::FLOAT;
  v.id = id;
  v.f = f;
  sink(ctx, v);
}

void emit_bool(Decoder::Sink sink, void *ctx, uint16_t id, bool b) {
  DecodedValue v{};
  v.kind = DecodedValue::Kind::BOOL;
  v.id = id;
  v.b = b;
  sink(ctx, v);
}

void emit_uint(Decoder::Sink sink, void *ctx, uint16_t id, uint32_t u) {
  DecodedValue v{};
  v.kind = DecodedValue::Kind::UINT;
  v.id = id;
  v.u = u;
  sink(ctx, v);
}

void emit_text(Decoder::Sink sink, void *ctx, uint16_t id, const char *text) {
  DecodedValue v{};
  v.kind = DecodedValue::Kind::TEXT;
  v.id = id;
  std::strncpy(v.text, text, sizeof(v.text) - 1);
  sink(ctx, v);
}

}  // namespace

// A known header with no payload is the request/ack side of a READ/WRITE
// transaction, not an anomaly: nothing to decode, nothing to reject.
bool Decoder::check_length(const Frame &f, uint8_t expected) const {
  if (!f.has_payload())
    return false;
  // The catalogued length must both be declared and actually be present: a
  // fixed-offset codec reading past what the frame carried would publish
  // uninitialised buffer contents as a measurement.
  return f.payload_len() == expected && f.buffered_payload_len() >= expected;
}

// The identity text fields are fixed-width and NUL-padded, and the
// width is catalogued but not guaranteed across firmware revisions. Gate on the
// two things that are genuinely unsafe to get wrong - reading past what was
// buffered, and a string too long for DecodedValue::text (which decode_text
// would otherwise silently clip) - and let decode_text's trailing-NUL check be
// the real structural gate. A width that merely disagrees with the catalogue is
// published.
bool Decoder::check_text_length(const Frame &f, uint8_t *len_out) const {
  if (!f.has_payload())
    return false;
  uint8_t declared = f.payload_len();
  if (declared > f.buffered_payload_len() || declared > sizeof(DecodedValue::text) - 1)
    return false;
  *len_out = declared;
  return true;
}

void Decoder::emit_text_field(const Frame &f, Sink sink, void *ctx, uint16_t id) const {
  uint8_t len = 0;
  if (!check_text_length(f, &len))
    return;
  char text[sizeof(DecodedValue::text)];
  if (!codec::decode_text(f.payload(), len, text, sizeof(text)))
    return;  // no trailing NUL: structurally invalid, never published
  emit_text(sink, ctx, id, text);
}

void Decoder::emit_minmax(const Frame &f, Sink sink, void *ctx, uint16_t min_id, uint16_t max_id) const {
  if (!check_length(f, 5))
    return;
  float min_c, max_c;
  if (codec::decode_minmax(f.payload(), &min_c, &max_c)) {
    emit_float(sink, ctx, min_id, min_c);
    emit_float(sink, ctx, max_id, max_c);
  }
}

void Decoder::emit_cycle(const Frame &f, Sink sink, void *ctx, uint16_t active_id, uint16_t count_id) const {
  if (!check_length(f, 12))
    return;
  codec::Cycle c = codec::decode_cycle(f.payload());
  emit_bool(sink, ctx, active_id, c.active);
  emit_uint(sink, ctx, count_id, c.count);
}

void Decoder::decode(const Frame &f, Sink sink, void *ctx) const {
  if (!f.crc_valid())
    return;  // never decode a frame whose CRC failed

  const uint8_t *p = f.payload();

  switch (f.header_key()) {
    case header::FIRMWARE_VERSION:
      emit_text_field(f, sink, ctx, ENT_FIRMWARE_VERSION);
      break;
    case header::SERIAL_NUMBER:
      emit_text_field(f, sink, ctx, ENT_SERIAL_NUMBER);
      break;
    case header::POWER_BOARD_VERSION:
      emit_text_field(f, sink, ctx, ENT_POWER_BOARD_VERSION);
      break;
    case header::CONTROLLER_MODEL:
      emit_text_field(f, sink, ctx, ENT_CONTROLLER_MODEL);
      break;
    case header::SETPOINT: {
      if (!check_length(f, 2))
        break;
      emit_float(sink, ctx, ENT_SETPOINT, codec::decode_temp(p));
      break;
    }
    case header::TEMPERATURES: {
      if (!check_length(f, 12))
        break;
      static constexpr uint16_t ids[6] = {ENT_WATER_TEMPERATURE,      ENT_COMPRESSOR_OUTLET_TEMPERATURE,
                                           ENT_AIR_INLET_TEMPERATURE, ENT_EVAPORATOR_1_TEMPERATURE,
                                           ENT_EVAPORATOR_2_TEMPERATURE, ENT_EVAPORATOR_3_TEMPERATURE};
      for (int i = 0; i < 6; i++)
        emit_float(sink, ctx, ids[i], codec::decode_temp(p + i * 2));
      break;
    }
    case header::WATER_TEMPERATURE_MINMAX:
      emit_minmax(f, sink, ctx, ENT_WATER_TEMPERATURE_MIN, ENT_WATER_TEMPERATURE_MAX);
      break;
    case header::COMPRESSOR_OUTLET_TEMPERATURE_MINMAX:
      emit_minmax(f, sink, ctx, ENT_COMPRESSOR_OUTLET_TEMPERATURE_MIN, ENT_COMPRESSOR_OUTLET_TEMPERATURE_MAX);
      break;
    case header::AIR_INLET_TEMPERATURE_MINMAX:
      emit_minmax(f, sink, ctx, ENT_AIR_INLET_TEMPERATURE_MIN, ENT_AIR_INLET_TEMPERATURE_MAX);
      break;
    case header::EVAPORATOR_1_TEMPERATURE_MINMAX:
      emit_minmax(f, sink, ctx, ENT_EVAPORATOR_1_TEMPERATURE_MIN, ENT_EVAPORATOR_1_TEMPERATURE_MAX);
      break;
    case header::EVAPORATOR_2_TEMPERATURE_MINMAX:
      emit_minmax(f, sink, ctx, ENT_EVAPORATOR_2_TEMPERATURE_MIN, ENT_EVAPORATOR_2_TEMPERATURE_MAX);
      break;
    case header::EVAPORATOR_3_TEMPERATURE_MINMAX:
      emit_minmax(f, sink, ctx, ENT_EVAPORATOR_3_TEMPERATURE_MIN, ENT_EVAPORATOR_3_TEMPERATURE_MAX);
      break;
    case header::CYCLE_1:
      emit_cycle(f, sink, ctx, ENT_CYCLE_1_ACTIVE, ENT_CYCLE_1_COUNT);
      break;
    case header::CYCLE_2:
      emit_cycle(f, sink, ctx, ENT_CYCLE_2_ACTIVE, ENT_CYCLE_2_COUNT);
      break;
    case header::CYCLE_3:
      emit_cycle(f, sink, ctx, ENT_CYCLE_3_ACTIVE, ENT_CYCLE_3_COUNT);
      break;
    case header::CYCLE_4:
      emit_cycle(f, sink, ctx, ENT_CYCLE_4_ACTIVE, ENT_CYCLE_4_COUNT);
      break;
    case header::CYCLE_5:
      emit_cycle(f, sink, ctx, ENT_CYCLE_5_ACTIVE, ENT_CYCLE_5_COUNT);
      break;
    case header::CYCLE_6:
      emit_cycle(f, sink, ctx, ENT_CYCLE_6_ACTIVE, ENT_CYCLE_6_COUNT);
      break;
    case header::INPUT_STATUS: {
      if (!check_length(f, 3))
        break;
      emit_bool(sink, ctx, ENT_INPUT_I2, codec::decode_bool(p));
      emit_bool(sink, ctx, ENT_INPUT_I1, codec::decode_bool(p + 1));
      emit_bool(sink, ctx, ENT_HEATING_ACTIVE, codec::decode_bool(p + 2));
      break;
    }
    case header::HMI_VERSION:
      emit_text_field(f, sink, ctx, ENT_HMI_VERSION);
      break;
    case header::HMI_MODEL:
      emit_text_field(f, sink, ctx, ENT_HMI_MODEL);
      break;
    case header::HEATING_ELEMENT: {
      if (!check_length(f, 1))
        break;
      // Wire value is 00 or 64 (= 100); only ever seen at those two, so the
      // possible percentage is published as a bool. See docs/protocol.md.
      emit_bool(sink, ctx, ENT_HEATING_ELEMENT_ACTIVE, codec::decode_bool(p));
      break;
    }
    default:
      if (is_unmapped_header(f.header_key()))
        stats_.unmapped_frames++;
      else
        stats_.unknown_headers++;
      break;
  }
}

}  // namespace atlantic_v5
