// L1 core. Host-compilable: no esp-idf, no ESPHome, no Arduino, no exceptions, no heap.
#pragma once

#include <atomic>

#include "frame.h"
#include "types.h"

namespace atlantic_v5 {

// The only rewrite the codebase is allowed to perform (plan 2.8: the input-status
// frame). control_mode() is written from the main loop and read from the relay
// task/thread, hence the atomic rather than a lock.
class RelayPolicy {
 public:
  static constexpr uint64_t INPUT_STATUS_HEADER_KEY = 0x0164FF1403ULL;
  static constexpr uint8_t INPUT_STATUS_PAYLOAD_LEN = REWRITE_PAYLOAD_LEN;

  void set_control_mode(ControlMode m) { mode_.store(static_cast<uint8_t>(m), std::memory_order_relaxed); }
  ControlMode control_mode() const {
    return static_cast<ControlMode>(mode_.load(std::memory_order_relaxed));
  }

  // Rewrites f's payload in place per 2.8 and returns true if it did so. A no-op
  // (returns false, f untouched) unless the header, payload length and CRC all
  // match exactly and the mode isn't PASSTHROUGH. Byte 2 (heating status) is
  // always copied through unchanged; it is a status report, not a command.
  bool apply(Frame &f) const;

 private:
  std::atomic<uint8_t> mode_{static_cast<uint8_t>(ControlMode::PASSTHROUGH)};
};

}  // namespace atlantic_v5
