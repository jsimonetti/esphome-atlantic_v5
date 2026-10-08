// L1 core. Host-compilable: no esp-idf, no ESPHome, no Arduino, no exceptions, no heap.
// Header constants + entity metadata for the message catalogue (docs/protocol.md
// "Message catalogue"). Kept next
// to the decoder so the catalogue and the code that implements it stay in one place.
#pragma once

#include <cstddef>
#include <cstdint>

namespace atlantic_v5 {

// Index into L3's entity table. Order mirrors the message catalogue in
// docs/protocol.md, payload-bearing frames with known meaning only, plus a block of
// diagnostic counters appended at the end - not part of the wire
// catalogue, but published through the same entity-id/publish plumbing.
enum EntityId : uint16_t {
  ENT_CONTROLLER_FIRMWARE_VERSION = 0,
  ENT_APPLIANCE_SERIAL,
  ENT_PRODUCT_CODE,
  ENT_POWER_BOARD_VERSION,
  // Assumed, not established - see header::ASSUMED.
  ENT_CONTROLLER_MODEL,
  // Assumed, not established - see header::ASSUMED.
  ENT_SETPOINT_MIN,
  ENT_SETPOINT_MAX,
  ENT_WATER_TEMPERATURE,
  ENT_COMPRESSOR_OUTLET_TEMPERATURE,
  ENT_AIR_INLET_TEMPERATURE,
  ENT_EVAPORATOR_1_TEMPERATURE,
  ENT_EVAPORATOR_2_TEMPERATURE,
  ENT_EVAPORATOR_3_TEMPERATURE,
  ENT_WATER_TEMPERATURE_MIN,
  ENT_WATER_TEMPERATURE_MAX,
  ENT_COMPRESSOR_OUTLET_TEMPERATURE_MIN,
  ENT_COMPRESSOR_OUTLET_TEMPERATURE_MAX,
  ENT_AIR_INLET_TEMPERATURE_MIN,
  ENT_AIR_INLET_TEMPERATURE_MAX,
  ENT_EVAPORATOR_1_TEMPERATURE_MIN,
  ENT_EVAPORATOR_1_TEMPERATURE_MAX,
  ENT_EVAPORATOR_2_TEMPERATURE_MIN,
  ENT_EVAPORATOR_2_TEMPERATURE_MAX,
  ENT_EVAPORATOR_3_TEMPERATURE_MIN,
  ENT_EVAPORATOR_3_TEMPERATURE_MAX,
  ENT_CYCLE_1_ACTIVE,
  ENT_CYCLE_1_COUNT,
  ENT_CYCLE_2_ACTIVE,
  ENT_CYCLE_2_COUNT,
  ENT_CYCLE_3_ACTIVE,
  ENT_CYCLE_3_COUNT,
  ENT_CYCLE_4_ACTIVE,
  ENT_CYCLE_4_COUNT,
  ENT_CYCLE_5_ACTIVE,
  ENT_CYCLE_5_COUNT,
  ENT_CYCLE_6_ACTIVE,
  ENT_CYCLE_6_COUNT,
  ENT_INPUT_I2,
  ENT_INPUT_I1,
  ENT_HEATING_ACTIVE,
  ENT_HMI_FIRMWARE_VERSION,
  // Assumed, not established - see header::ASSUMED.
  ENT_HMI_MODEL,
  ENT_HEATING_ELEMENT_ACTIVE,

  // Diagnostics, off by default. The three framing counters come in two
  // flavours and exactly one flavour is live per mode: the unsuffixed ones in
  // listener (one assembler, one wire), the _hmi/_main variants in mitm (one
  // assembler per side, never summed - see README "Diagnostics").
  ENT_VALID_FRAMES,
  ENT_CRC_ERRORS,
  ENT_DROPPED_BYTES,
  ENT_VALID_FRAMES_HMI,
  ENT_CRC_ERRORS_HMI,
  ENT_DROPPED_BYTES_HMI,
  ENT_VALID_FRAMES_MAIN,
  ENT_CRC_ERRORS_MAIN,
  ENT_DROPPED_BYTES_MAIN,
  ENT_UNKNOWN_FRAMES,
  ENT_FRAMES_RELAYED,
  ENT_REWRITES_APPLIED,
  ENT_QUEUE_OVERFLOWS,
  ENT_RELAY_LATENCY_MAX_US,
  ENT_RELAY_LATENCY_AVG_US,
  ENT_TASK_STACK_FREE,
  // Not wire traffic either: the staleness gate's own state.
  ENT_CONNECTED,
  ENT_COUNT
};

// The JSON/entity key for id, per the catalogue's Entity column. Returns
// "unknown" for anything outside [0, ENT_COUNT).
const char *entity_name(EntityId id);

// Header keys for payload-bearing frames with known meaning.
namespace header {
constexpr uint64_t CONTROLLER_FIRMWARE_VERSION = 0x0164006401ULL;
constexpr uint64_t PRODUCT_CODE = 0x0164006501ULL;
constexpr uint64_t APPLIANCE_SERIAL = 0x0164006601ULL;
constexpr uint64_t POWER_BOARD_VERSION = 0x0164006701ULL;
constexpr uint64_t CONTROLLER_MODEL = 0x0164006E01ULL;
// The two ends of the range the HMI lets the user dial the tank setpoint
// between, not the setpoint itself - MAIN states them once per init.
constexpr uint64_t SETPOINT_MIN = 0x016414B701ULL;
constexpr uint64_t SETPOINT_MAX = 0x0164158301ULL;
constexpr uint64_t TEMPERATURES = 0x0164FEB006ULL;
constexpr uint64_t WATER_TEMPERATURE_MINMAX = 0x0164FEBA03ULL;
constexpr uint64_t COMPRESSOR_OUTLET_TEMPERATURE_MINMAX = 0x0164FEBD03ULL;
constexpr uint64_t AIR_INLET_TEMPERATURE_MINMAX = 0x0164FEC003ULL;
constexpr uint64_t EVAPORATOR_1_TEMPERATURE_MINMAX = 0x0164FEC303ULL;
constexpr uint64_t EVAPORATOR_2_TEMPERATURE_MINMAX = 0x0164FEC603ULL;
constexpr uint64_t EVAPORATOR_3_TEMPERATURE_MINMAX = 0x0164FEC903ULL;
constexpr uint64_t CYCLE_1 = 0x0164FEE203ULL;
constexpr uint64_t CYCLE_2 = 0x0164FEE503ULL;
constexpr uint64_t CYCLE_3 = 0x0164FEE803ULL;
constexpr uint64_t CYCLE_4 = 0x0164FEEB03ULL;
constexpr uint64_t CYCLE_5 = 0x0164FEEE03ULL;
constexpr uint64_t CYCLE_6 = 0x0164FEF103ULL;
constexpr uint64_t INPUT_STATUS = 0x0164FF1403ULL;
constexpr uint64_t HMI_FIRMWARE_VERSION = 0x0165000301ULL;
constexpr uint64_t HMI_MODEL = 0x0165000A01ULL;
constexpr uint64_t HEATING_ELEMENT = 0x0165FEF701ULL;

// Subset of MAPPED whose meaning is a working hypothesis rather than an
// established fact (docs/protocol.md "Assumed mappings"). These derive entities
// exactly like any other mapped header - the array exists only so "which
// meanings has this project actually established?" stays answerable in code.
// Ascending, and every member must also appear in MAPPED.
constexpr uint64_t ASSUMED[] = {
    CONTROLLER_MODEL,
    SETPOINT_MIN,
    SETPOINT_MAX,
    HMI_MODEL,
    HEATING_ELEMENT,
};
constexpr size_t ASSUMED_COUNT = sizeof(ASSUMED) / sizeof(ASSUMED[0]);

// Every mapped key above, ascending. Kept as a plain key array (rather than
// only as named constants) so classify_header can binary-search it and the
// disjointness assert below has a walkable list. Not read by decode(), which
// dispatches on its own switch.
constexpr uint64_t MAPPED[] = {
    CONTROLLER_FIRMWARE_VERSION,
    PRODUCT_CODE,
    APPLIANCE_SERIAL,
    POWER_BOARD_VERSION,
    CONTROLLER_MODEL,
    SETPOINT_MIN,
    SETPOINT_MAX,
    TEMPERATURES,
    WATER_TEMPERATURE_MINMAX,
    COMPRESSOR_OUTLET_TEMPERATURE_MINMAX,
    AIR_INLET_TEMPERATURE_MINMAX,
    EVAPORATOR_1_TEMPERATURE_MINMAX,
    EVAPORATOR_2_TEMPERATURE_MINMAX,
    EVAPORATOR_3_TEMPERATURE_MINMAX,
    CYCLE_1,
    CYCLE_2,
    CYCLE_3,
    CYCLE_4,
    CYCLE_5,
    CYCLE_6,
    INPUT_STATUS,
    HMI_FIRMWARE_VERSION,
    HMI_MODEL,
    HEATING_ELEMENT,
};
constexpr size_t MAPPED_COUNT = sizeof(MAPPED) / sizeof(MAPPED[0]);

// docs/protocol.md "Unmapped messages": headers that are known, expected,
// routine traffic whose meaning has not been established, plus the two
// payload-less init headers listed under that table. Ascending, so
// classify_header can binary-search it.
constexpr uint64_t UNMAPPED[] = {
    0x0164007001ULL, 0x0164007101ULL, 0x0164007501ULL, 0x01640165FEULL, 0x0164152A01ULL, 0x016421B601ULL,
    0x016443130DULL, 0x0164FDED01ULL, 0x0164FDEE01ULL, 0x0164FDFA01ULL, 0x0164FDFD01ULL, 0x0164FE0001ULL,
    0x0164FED801ULL, 0x0164FFDC01ULL, 0x0165152301ULL, 0x016516B301ULL, 0x0165FDF802ULL, 0x0165FDFB02ULL,
    0x0165FDFE02ULL, 0x0165FEF901ULL, 0x0165FEFB01ULL, 0x0165FEFD01ULL, 0x0165FEFF01ULL, 0x0165FF0101ULL,
    0x0165FF0301ULL,
};
constexpr size_t UNMAPPED_COUNT = sizeof(UNMAPPED) / sizeof(UNMAPPED[0]);

namespace detail {
constexpr bool ascending(const uint64_t *keys, size_t n) {
  for (size_t i = 1; i < n; i++)
    if (keys[i - 1] >= keys[i])
      return false;
  return true;
}
constexpr bool disjoint(const uint64_t *a, size_t an, const uint64_t *b, size_t bn) {
  for (size_t i = 0; i < an; i++)
    for (size_t j = 0; j < bn; j++)
      if (a[i] == b[j])
        return false;
  return true;
}
constexpr bool subset(const uint64_t *a, size_t an, const uint64_t *b, size_t bn) {
  for (size_t i = 0; i < an; i++) {
    bool found = false;
    for (size_t j = 0; j < bn; j++)
      if (a[i] == b[j])
        found = true;
    if (!found)
      return false;
  }
  return true;
}
}  // namespace detail

static_assert(detail::ascending(MAPPED, MAPPED_COUNT), "header::MAPPED must be strictly ascending");
static_assert(detail::ascending(UNMAPPED, UNMAPPED_COUNT), "header::UNMAPPED must be strictly ascending");
static_assert(detail::ascending(ASSUMED, ASSUMED_COUNT), "header::ASSUMED must be strictly ascending");
static_assert(detail::disjoint(MAPPED, MAPPED_COUNT, UNMAPPED, UNMAPPED_COUNT),
              "a header key cannot be both mapped and unmapped");
static_assert(detail::subset(ASSUMED, ASSUMED_COUNT, MAPPED, MAPPED_COUNT),
              "every header::ASSUMED key must also be in header::MAPPED");
}  // namespace header

// Where a header key sits in the message catalogue (CONTEXT.md).
enum class HeaderClass : uint8_t {
  MAPPED,    // known meaning, derives entities
  UNMAPPED,  // known, expected traffic with no established meaning
  UNKNOWN,   // absent from the catalogue entirely
};

// The one lookup that answers the full three-way split, by header key alone:
// which of the two tables, if either, holds key.
HeaderClass classify_header(uint64_t key);

// True for a header key in header::UNMAPPED: known, expected traffic with no
// established meaning. A frame whose key is in neither table is an *unknown*
// frame, the only kind that warrants operator attention (CONTEXT.md).
bool is_unmapped_header(uint64_t key);

}  // namespace atlantic_v5
