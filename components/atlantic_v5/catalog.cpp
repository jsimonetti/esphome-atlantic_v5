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
    "valid_frames",
    "crc_errors",
    "dropped_bytes",
    "valid_frames_hmi",
    "crc_errors_hmi",
    "dropped_bytes_hmi",
    "valid_frames_main",
    "crc_errors_main",
    "dropped_bytes_main",
    "unknown_frames",
    "length_mismatches",
    "text_length_variants",
    "frames_relayed",
    "rewrites_applied",
    "echo_bytes",
    "queue_overflows",
    "relay_latency_max_us",
    "relay_latency_avg_us",
    "task_stack_free",
    "last_unknown_frame",
    "last_frame_dump",
    "self_test_result",
    "connected",
};
}  // namespace

const char *entity_name(EntityId id) {
  return id < ENT_COUNT ? NAMES[id] : "unknown";
}

bool is_unmapped_header(uint64_t key) {
  size_t lo = 0, hi = header::UNMAPPED_COUNT;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (header::UNMAPPED[mid] == key)
      return true;
    if (header::UNMAPPED[mid] < key)
      lo = mid + 1;
    else
      hi = mid;
  }
  return false;
}

}  // namespace atlantic_v5
