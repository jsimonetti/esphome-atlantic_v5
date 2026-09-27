#include "catalog.h"

namespace atlantic_v5 {

namespace {
constexpr const char *NAMES[ENT_COUNT] = {
    "firmware_version",
    "serial_number",
    "power_board_version",
    "controller_model",
    "setpoint",
    "water_temperature",
    "compressor_outlet_temperature",
    "air_inlet_temperature",
    "evaporator_1_temperature",
    "evaporator_2_temperature",
    "evaporator_3_temperature",
    "water_temperature_min",
    "water_temperature_max",
    "compressor_outlet_temperature_min",
    "compressor_outlet_temperature_max",
    "air_inlet_temperature_min",
    "air_inlet_temperature_max",
    "evaporator_1_temperature_min",
    "evaporator_1_temperature_max",
    "evaporator_2_temperature_min",
    "evaporator_2_temperature_max",
    "evaporator_3_temperature_min",
    "evaporator_3_temperature_max",
    "cycle_1_active",
    "cycle_1_count",
    "cycle_2_active",
    "cycle_2_count",
    "cycle_3_active",
    "cycle_3_count",
    "cycle_4_active",
    "cycle_4_count",
    "cycle_5_active",
    "cycle_5_count",
    "cycle_6_active",
    "cycle_6_count",
    "input_i2",
    "input_i1",
    "heating_active",
    "hmi_version",
    "hmi_model",
};
}  // namespace

const char *entity_name(EntityId id) {
  return id < ENT_COUNT ? NAMES[id] : "unknown";
}

}  // namespace atlantic_v5
