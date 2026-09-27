// L1 core type. Host-compilable: no esp-idf, no ESPHome, no Arduino, no exceptions, no heap.
#pragma once

#include <cstddef>
#include <cstdint>

namespace atlantic_v5 {

static constexpr size_t MAX_FRAME = 32;  // 29 bytes observed on the wire, rounded up
static constexpr size_t HEADER_LEN = 5;
static constexpr uint32_t BAUD = 38400;

// Which party a byte stream or Frame came from. BUS is only meaningful for a
// single-wire listener capture, where direction isn't separable at the wire.
enum class Channel : uint8_t { HMI = 0, MAIN = 1, BUS = 2 };

// Channel narrowed to {HMI, MAIN}: the two physical BusIo sides in MITM mode.
using Side = Channel;

// A transport-layer Side's current pin-drive state. Not to be confused with
// the message catalogue's Origin (M/H), which is a property of frame data.
enum class LineMode : uint8_t { RX = 0, TX = 1 };

// What crosses the relay-task -> main-loop thread boundary.
struct FrameEvent {
  uint8_t data[MAX_FRAME];
  uint8_t len;
  Channel channel;
  bool modified;
  bool crc_ok;
  uint32_t t_us;  // arrival of the last byte
};

enum class ControlMode : uint8_t {
  PASSTHROUGH = 0,
  NORMAL = 1,
  EAGER = 2,
  OFF = 3,
  BOOST = 4,
};

}  // namespace atlantic_v5
