#include "retained_payload.h"

#include <cmath>
#include <cstring>

#include "v40.h"

namespace atlantic_v5 {

namespace {
constexpr uint8_t FLAG_INIT = 0x01;
constexpr uint8_t FLAG_COLD = 0x02;
constexpr uint16_t FLAG_BYTES = 1;
constexpr uint16_t LEN_BYTES = 2;
constexpr uint16_t COLD_BYTES = 4;
}  // namespace

uint16_t RetainedPayload::encode(const InitValueStore &init, const float *cold, uint8_t *out, uint16_t cap) {
  if (cap < FLAG_BYTES)
    return 0;
  uint16_t n = FLAG_BYTES;
  uint8_t flags = 0;

  if (cap >= n + LEN_BYTES) {
    const uint16_t init_len = init.encode(out + n + LEN_BYTES, static_cast<uint16_t>(cap - n - LEN_BYTES));
    if (init_len > 0) {
      out[n] = static_cast<uint8_t>(init_len);
      out[n + 1] = static_cast<uint8_t>(init_len >> 8);
      n = static_cast<uint16_t>(n + LEN_BYTES + init_len);
      flags |= FLAG_INIT;
    }
  }

  if (cold != nullptr) {
    if (n + COLD_BYTES > cap)
      return 0;
    std::memcpy(out + n, cold, COLD_BYTES);
    n = static_cast<uint16_t>(n + COLD_BYTES);
    flags |= FLAG_COLD;
  }

  if (flags == 0)
    return 0;
  out[0] = flags;
  return n;
}

bool RetainedPayload::decode(const uint8_t *data, uint16_t len, InitValueStore &init, bool &has_cold, float &cold) {
  InitValueStore parsed;

  if (data == nullptr || len < FLAG_BYTES)
    return false;
  const uint8_t flags = data[0];
  if (flags == 0 || (flags & ~(FLAG_INIT | FLAG_COLD)) != 0)
    return false;

  uint16_t n = FLAG_BYTES;
  if (flags & FLAG_INIT) {
    if (n + LEN_BYTES > len)
      return false;
    const uint16_t init_len = static_cast<uint16_t>(data[n] | (data[n + 1] << 8));
    n = static_cast<uint16_t>(n + LEN_BYTES);
    if (n + init_len > len || !parsed.decode(data + n, init_len))
      return false;
    n = static_cast<uint16_t>(n + init_len);
  }

  float parsed_cold = 0;
  if (flags & FLAG_COLD) {
    if (n + COLD_BYTES > len)
      return false;
    std::memcpy(&parsed_cold, data + n, COLD_BYTES);
    n = static_cast<uint16_t>(n + COLD_BYTES);
    if (!std::isfinite(parsed_cold) || parsed_cold < COLD_WATER_MIN_C || parsed_cold > COLD_WATER_MAX_C)
      return false;
  }

  if (n != len)
    return false;

  init = parsed;
  has_cold = (flags & FLAG_COLD) != 0;
  if (has_cold)
    cold = parsed_cold;
  return true;
}

}  // namespace atlantic_v5
