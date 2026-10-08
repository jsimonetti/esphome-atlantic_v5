// Core test: replay CLI output vs. frozen golden JSON, for
// every available real capture. Reuses
// replay_lib directly rather than shelling out to the replay binary.
//
// Only test/captures/real_*.csv may back a golden file here (see that
// directory's README.md): synthetic_*.csv and external_*.csv must never be used to
// freeze *.expected.json, so this test intentionally covers only the real
// capture(s) that exist.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "replay_lib.h"
#include "test_harness.h"

#ifndef CAPTURES_DIR
#define CAPTURES_DIR "test/captures"
#endif

namespace {

std::string read_file(const std::string &path) {
  std::ifstream in(path);
  CHECK(in.is_open());
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

int main() {
  replay::ChannelOf single_bus = [](const std::string & /*name*/) { return atlantic_v5::Channel::BUS; };
  replay::ChannelOf dual_bus = [](const std::string &name) {
    if (name == "hmi")
      return atlantic_v5::Channel::HMI;
    if (name == "main")
      return atlantic_v5::Channel::MAIN;
    return atlantic_v5::Channel::BUS;
  };

  for (const char *name : {"real_single_bus_version_poll", "real_single_bus_idle_polling"}) {
    const std::string stem = std::string(CAPTURES_DIR) + "/" + name;
    auto rows = replay::load_capture(stem + ".csv", single_bus);
    CHECK(!rows.empty());
    auto lines = replay::replay_single_bus(rows);

    std::string actual;
    for (const auto &line : lines)
      actual += line + "\n";

    // The harness prints no message, so name the capture before comparing;
    // otherwise a failure here is just a line number shared by both captures.
    std::string expected = read_file(stem + ".expected.json");
    if (actual != expected)
      std::printf("golden mismatch for %s\n", name);
    CHECK(actual == expected);
  }

  for (const char *name : {"real_dual_bus_idle_polling", "real_dual_bus_link_blackout"}) {
    const std::string stem = std::string(CAPTURES_DIR) + "/" + name;
    auto rows = replay::load_capture(stem + ".csv", dual_bus);
    CHECK(!rows.empty());
    auto lines = replay::replay_dual_bus(rows);

    std::string actual;
    for (const auto &line : lines)
      actual += line + "\n";

    std::string expected = read_file(stem + ".expected.json");
    if (actual != expected)
      std::printf("golden mismatch for %s\n", name);
    CHECK(actual == expected);
  }

  TEST_MAIN_RETURN();
}
