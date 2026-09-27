// L1 core. Host-compilable: no esp-idf, no ESPHome, no Arduino, no exceptions, no heap.
// Header constants + entity metadata for the message catalogue (plan 2.7). Kept next
// to the decoder so the catalogue and the code that implements it stay in one place.
#pragma once

#include <cstdint>

namespace atlantic_v5 {

// Index into L3's entity table (plan 3.3). Order mirrors the message catalogue
// (plan 2.7), payload-bearing frames with known meaning only.
enum EntityId : uint16_t {
  ENT_FIRMWARE_VERSION = 0,
  ENT_SERIAL_NUMBER,
  ENT_POWER_BOARD_VERSION,
  ENT_CONTROLLER_MODEL,
  ENT_SETPOINT,
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
  ENT_HMI_VERSION,
  ENT_HMI_MODEL,
  ENT_COUNT
};

// The JSON/entity key for id, per the catalogue's Entity column (2.7). Returns
// "unknown" for anything outside [0, ENT_COUNT).
const char *entity_name(EntityId id);

// Header keys for payload-bearing frames with known meaning (plan 2.7).
namespace header {
constexpr uint64_t FIRMWARE_VERSION = 0x0164006401ULL;
constexpr uint64_t SERIAL_NUMBER = 0x0164006601ULL;
constexpr uint64_t POWER_BOARD_VERSION = 0x0164006701ULL;
constexpr uint64_t CONTROLLER_MODEL = 0x0164006E01ULL;
constexpr uint64_t SETPOINT = 0x016414B701ULL;
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
constexpr uint64_t HMI_VERSION = 0x0165000301ULL;
constexpr uint64_t HMI_MODEL = 0x0165000A01ULL;
}  // namespace header

}  // namespace atlantic_v5
