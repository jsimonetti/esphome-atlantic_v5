// Shared logic behind the capture replay CLI (plan Appendix A): capture loading,
// mode-appropriate framing, and one JSON line per delivered frame. Factored out
// of replay.cpp so a host test can exercise the exact same code path in-process
// and assert its output against a golden file, without shelling out.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/types.h"

namespace replay {

struct Row {
  uint32_t t_us;
  std::string channel_name;
  atlantic_v5::Channel channel;
  std::vector<uint8_t> bytes;
};

using ChannelOf = std::function<atlantic_v5::Channel(const std::string &)>;

// Loads a capture CSV (t_us,channel,hex per line, '#'-prefixed comments
// skipped). channel_of maps a capture's raw channel field to the Channel enum
// (hmi/main for dual-bus captures, whatever name the listener capture uses
// for single-bus ones); anything channel_of doesn't recognise becomes BUS.
std::vector<Row> load_capture(const std::string &path, const ChannelOf &channel_of);

// Assembles rows through per-side FrameAssemblers (plan 2.5.1) and decodes
// each delivered frame (plan 3.3), returning one Appendix A JSON line per
// frame in delivery order.
std::vector<std::string> replay_dual_bus(const std::vector<Row> &rows);

// Assembles rows through a single BUS FrameAssembler (plan 2.5.2, speculative
// CRC) and decodes each delivered frame, returning one JSON line per frame.
std::vector<std::string> replay_single_bus(const std::vector<Row> &rows);

}  // namespace replay
