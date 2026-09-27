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

  auto rows = replay::load_capture(std::string(CAPTURES_DIR) + "/real_single_bus_version_poll.csv", single_bus);
  CHECK(!rows.empty());
  auto lines = replay::replay_single_bus(rows);

  std::string actual;
  for (const auto &line : lines)
    actual += line + "\n";

  std::string expected = read_file(std::string(CAPTURES_DIR) + "/real_single_bus_version_poll.expected.json");
  CHECK(actual == expected);

  TEST_MAIN_RETURN();
}
