#include "init_values.h"

#include <cstring>

namespace atlantic_v5 {

namespace {

bool valid_kind(uint8_t raw) {
  switch (static_cast<DecodedValue::Kind>(raw)) {
    case DecodedValue::Kind::FLOAT:
    case DecodedValue::Kind::BOOL:
    case DecodedValue::Kind::TEXT:
    case DecodedValue::Kind::UINT:
      return true;
  }
  return false;
}

// Compares only the member the kind selects: DecodedValue's other members keep
// whatever the decoder left in them, so a memcmp would report spurious changes.
bool same_value(const DecodedValue &a, const DecodedValue &b) {
  if (a.kind != b.kind)
    return false;
  switch (a.kind) {
    case DecodedValue::Kind::FLOAT:
      return a.f == b.f;
    case DecodedValue::Kind::BOOL:
      return a.b == b.b;
    case DecodedValue::Kind::UINT:
      return a.u == b.u;
    case DecodedValue::Kind::TEXT:
      return std::strncmp(a.text, b.text, sizeof(a.text)) == 0;
  }
  return false;
}

void write_u32(uint8_t *out, uint32_t v) {
  out[0] = static_cast<uint8_t>(v);
  out[1] = static_cast<uint8_t>(v >> 8);
  out[2] = static_cast<uint8_t>(v >> 16);
  out[3] = static_cast<uint8_t>(v >> 24);
}

uint32_t read_u32(const uint8_t *in) {
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
         (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}

// Floats travel as their IEEE-754 bit pattern in the same little-endian order
// the integers use, rather than as a raw object copy, so the encoding is a
// documented byte layout rather than whatever the compiler happens to lay out.
uint32_t float_bits(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(bits));
  return bits;
}

float bits_float(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

uint8_t value_len(const DecodedValue &v) {
  switch (v.kind) {
    case DecodedValue::Kind::FLOAT:
    case DecodedValue::Kind::UINT:
      return 4;
    case DecodedValue::Kind::BOOL:
      return 1;
    case DecodedValue::Kind::TEXT: {
      // Bounded rather than strlen: a DecodedValue is only as terminated as
      // whoever filled it, and the field is the same width either way.
      uint8_t n = 0;
      while (n < InitValueStore::MAX_VALUE_BYTES && v.text[n] != '\0')
        n++;
      return n;
    }
  }
  return 0;
}

}  // namespace

InitValueStore::Observation InitValueStore::observe(const DecodedValue &v) {
  const size_t slot = init_cadence_index(v.id);
  if (slot == INIT_CADENCE_COUNT)
    return Observation::NOT_INIT_CADENCE;
  if (this->present_[slot] && same_value(this->values_[slot], v))
    return Observation::UNCHANGED;
  this->values_[slot] = v;
  this->present_[slot] = true;
  return Observation::CHANGED;
}

uint16_t InitValueStore::encode(uint8_t *out, uint16_t cap) const {
  if (cap < COUNT_BYTES)
    return 0;
  uint16_t n = COUNT_BYTES;
  uint8_t count = 0;

  for (size_t i = 0; i < INIT_CADENCE_COUNT; i++) {
    if (!this->present_[i])
      continue;
    const DecodedValue &v = this->values_[i];
    const uint8_t len = value_len(v);
    if (n + RECORD_HEADER_BYTES + len > cap)
      return 0;
    out[n++] = static_cast<uint8_t>(INIT_CADENCE[i]);
    out[n++] = static_cast<uint8_t>(INIT_CADENCE[i] >> 8);
    out[n++] = static_cast<uint8_t>(v.kind);
    out[n++] = len;
    switch (v.kind) {
      case DecodedValue::Kind::FLOAT:
        write_u32(out + n, float_bits(v.f));
        break;
      case DecodedValue::Kind::UINT:
        write_u32(out + n, v.u);
        break;
      case DecodedValue::Kind::BOOL:
        out[n] = v.b ? 1 : 0;
        break;
      case DecodedValue::Kind::TEXT:
        std::memcpy(out + n, v.text, len);
        break;
    }
    n = static_cast<uint16_t>(n + len);
    count++;
  }

  if (count == 0)
    return 0;
  out[0] = count;
  return n;
}

bool InitValueStore::decode(const uint8_t *data, uint16_t len) {
  if (this->decode_records(data, len))
    return true;
  // A rejected block holds nothing at all: a prefix of valid records is still
  // bytes this firmware cannot vouch for.
  this->clear();
  return false;
}

void InitValueStore::clear() {
  for (bool &p : this->present_)
    p = false;
}

bool InitValueStore::decode_records(const uint8_t *data, uint16_t len) {
  this->clear();

  if (len < COUNT_BYTES)
    return false;
  const uint8_t count = data[0];
  if (count == 0 || count > INIT_CADENCE_COUNT)
    return false;

  uint16_t n = COUNT_BYTES;
  for (uint8_t i = 0; i < count; i++) {
    if (n + RECORD_HEADER_BYTES > len)
      return false;
    const uint16_t id = static_cast<uint16_t>(data[n] | (data[n + 1] << 8));
    const uint8_t kind = data[n + 2];
    const uint8_t value_bytes = data[n + 3];
    n = static_cast<uint16_t>(n + RECORD_HEADER_BYTES);
    if (n + value_bytes > len)
      return false;

    const size_t slot = init_cadence_index(id);
    // A record naming anything else, or naming the same entity twice, means
    // these bytes are not an encoding this firmware produced.
    if (slot == INIT_CADENCE_COUNT || this->present_[slot])
      return false;
    if (!valid_kind(kind))
      return false;

    DecodedValue v{};
    v.kind = static_cast<DecodedValue::Kind>(kind);
    v.id = id;
    switch (v.kind) {
      case DecodedValue::Kind::FLOAT:
        if (value_bytes != 4)
          return false;
        v.f = bits_float(read_u32(data + n));
        break;
      case DecodedValue::Kind::UINT:
        if (value_bytes != 4)
          return false;
        v.u = read_u32(data + n);
        break;
      case DecodedValue::Kind::BOOL:
        if (value_bytes != 1)
          return false;
        v.b = data[n] != 0;
        break;
      case DecodedValue::Kind::TEXT:
        if (value_bytes > MAX_VALUE_BYTES)
          return false;
        std::memcpy(v.text, data + n, value_bytes);
        v.text[value_bytes] = '\0';
        break;
    }
    n = static_cast<uint16_t>(n + value_bytes);
    this->values_[slot] = v;
    this->present_[slot] = true;
  }

  // Trailing bytes mean the encoding and the length disagree, which is the
  // same evidence as a malformed record.
  return n == len;
}

void InitValueStore::each(Decoder::Sink sink, void *ctx) const {
  for (size_t i = 0; i < INIT_CADENCE_COUNT; i++)
    if (this->present_[i])
      sink(ctx, this->values_[i]);
}

}  // namespace atlantic_v5
