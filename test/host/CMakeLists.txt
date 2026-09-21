cmake_minimum_required(VERSION 3.16)
project(atlantic_v5_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

# core/ is host-compilable by design (implementation plan, architecture rules):
# no esp-idf, no ESPHome, no Arduino headers, no heap in runtime paths.
add_library(atlantic_v5_core INTERFACE)
target_include_directories(atlantic_v5_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/atlantic_v5)

# Test binaries are added here as each milestone's core/ files land:
# test_crc (M1), test_assembler (M2), test_decoder (M3), test_relay (M5).
