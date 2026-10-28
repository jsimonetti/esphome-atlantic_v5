// L1 core. Host-compilable: no esp-idf, no ESPHome, no Arduino, no exceptions, no heap.
#pragma once

#include <atomic>

#include "frame.h"
#include "types.h"

namespace atlantic_v5 {

// The only rewrites the codebase is allowed to perform: the input-status frame
// (docs/protocol.md "Control surface") and the water-temperature field of the
// temperatures frame. control_mode() and temperature_override() are written from the
// main loop and read from the relay task/thread, hence atomics rather than a lock.
class RelayPolicy {
 public:
  static constexpr uint64_t INPUT_STATUS_HEADER_KEY = 0x0164FF1403ULL;
  static constexpr uint8_t INPUT_STATUS_PAYLOAD_LEN = REWRITE_PAYLOAD_LEN;
  static constexpr uint64_t TEMPERATURES_HEADER_KEY = 0x0164FEB006ULL;
  static constexpr uint8_t TEMPERATURES_PAYLOAD_LEN = 12;
  static_assert(TEMPERATURES_PAYLOAD_LEN <= OBSERVED_PAYLOAD_MAX, "observed snapshot must hold the temperatures payload");

  // Water temperature override in 0.01 degC wire units; 0 means no override.
  void set_temperature_override(int16_t centi_c) { temp_override_.store(centi_c, std::memory_order_relaxed); }
  int16_t temperature_override() const { return temp_override_.load(std::memory_order_relaxed); }

  void set_control_mode(ControlMode m) { mode_.store(static_cast<uint8_t>(m), std::memory_order_relaxed); }
  ControlMode control_mode() const {
    return static_cast<ControlMode>(mode_.load(std::memory_order_relaxed));
  }

  // Rewrites f's payload in place and returns true if it did so. A no-op
  // (returns false, f untouched) unless the header, payload length and CRC all
  // match exactly and a rewrite applies (non-PASSTHROUGH mode for input status,
  // non-zero override for temperatures). Input-status byte 2 (heating status) is
  // always copied through unchanged; it is a status report, not a command.
  bool apply(Frame &f) const;

 private:
  bool apply_input_status(Frame &f) const;
  bool apply_temperatures(Frame &f) const;

  std::atomic<uint8_t> mode_{static_cast<uint8_t>(ControlMode::PASSTHROUGH)};
  std::atomic<int16_t> temp_override_{0};
};

}  // namespace atlantic_v5
