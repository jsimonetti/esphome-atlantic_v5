// Capture replay CLI (plan Appendix A). Loads one or more capture CSVs,
// assembles + decodes frames per the chosen mode, and prints one JSON object
// per delivered frame to stdout, in input order.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/types.h"
#include "replay_lib.h"

namespace {

void print_usage() {
  std::fprintf(stderr,
               "usage: replay --capture PATH [--capture PATH ...] --mode {mitm,listener}\n"
               "              [--hmi-channel NAME] [--main-channel NAME] [--json]\n");
}

}  // namespace

int main(int argc, char **argv) {
  std::vector<std::string> captures;
  std::string mode;
  std::string hmi_channel = "hmi";
  std::string main_channel = "main";

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    auto next = [&](const char *flag) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s requires a value\n", flag);
        std::exit(1);
      }
      return argv[++i];
    };
    if (arg == "--capture") {
      captures.push_back(next("--capture"));
    } else if (arg == "--mode") {
      mode = next("--mode");
    } else if (arg == "--hmi-channel") {
      hmi_channel = next("--hmi-channel");
    } else if (arg == "--main-channel") {
      main_channel = next("--main-channel");
    } else if (arg == "--json") {
      continue;  // JSON is the only output format; accepted for Appendix A CLI compatibility
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      print_usage();
      return 1;
    }
  }

  if (captures.empty() || (mode != "mitm" && mode != "listener")) {
    print_usage();
    return 1;
  }

  std::vector<std::string> lines;
  if (mode == "mitm") {
    replay::ChannelOf classify = [&](const std::string &name) {
      if (name == hmi_channel)
        return atlantic_v5::Channel::HMI;
      if (name == main_channel)
        return atlantic_v5::Channel::MAIN;
      return atlantic_v5::Channel::BUS;
    };
    for (const auto &path : captures) {
      auto rows = replay::load_capture(path, classify);
      auto capture_lines = replay::replay_dual_bus(rows);
      lines.insert(lines.end(), capture_lines.begin(), capture_lines.end());
    }
  } else {
    replay::ChannelOf classify = [&](const std::string & /*name*/) { return atlantic_v5::Channel::BUS; };
    for (const auto &path : captures) {
      auto rows = replay::load_capture(path, classify);
      auto capture_lines = replay::replay_single_bus(rows);
      lines.insert(lines.end(), capture_lines.begin(), capture_lines.end());
    }
  }

  for (const auto &line : lines)
    std::printf("%s\n", line.c_str());

  return 0;
}
