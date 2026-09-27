cmake_minimum_required(VERSION 3.16)
project(atlantic_v5_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

# core/ (L1) and transport/ (L2) used to be subfolders of components/atlantic_v5/,
# but ESPHome's external-component loader only copies .cpp/.h files that are
# direct children of a component's package directory (it silently drops nested
# non-package subdirectories) - see build-and-tooling notes. Both layers now
# live flat in components/atlantic_v5/ alongside atlantic_v5.cpp/.h; the two
# CMake library targets below are still split to keep the L1/L2 dependency
# direction explicit.
add_library(atlantic_v5_core
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/atlantic_v5/crc16.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/atlantic_v5/frame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/atlantic_v5/assembler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/atlantic_v5/catalog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/atlantic_v5/decoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/atlantic_v5/relay_policy.cpp)
target_include_directories(atlantic_v5_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/atlantic_v5)

# L2: hardware-free pieces (Relay plan M5, Listener plan M4) are host-compilable
# against the abstract BusIo / core alone; only uart_bus_io.cpp (M6) will need
# esp-idf.
add_library(atlantic_v5_transport
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/atlantic_v5/relay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/atlantic_v5/listener.cpp)
target_link_libraries(atlantic_v5_transport PUBLIC atlantic_v5_core)

# Shared behind the replay CLI (Appendix A) and test_replay_golden, so the test
# exercises the exact code the CLI ships, not a reimplementation of it.
add_library(replay_lib replay_lib.cpp)
target_link_libraries(replay_lib PUBLIC atlantic_v5_core)
target_include_directories(replay_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Test binaries are added here as each milestone's core/ files land:
# test_crc (M1), test_assembler (M2), test_decoder (M3), test_relay (M5).
add_executable(test_crc test_crc.cpp)
target_link_libraries(test_crc PRIVATE atlantic_v5_core)
target_include_directories(test_crc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_crc PRIVATE
    CAPTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../captures")
add_test(NAME test_crc COMMAND test_crc)

add_executable(test_assembler test_assembler.cpp)
target_link_libraries(test_assembler PRIVATE atlantic_v5_core)
target_include_directories(test_assembler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_assembler PRIVATE
    CAPTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../captures")
add_test(NAME test_assembler COMMAND test_assembler)

add_executable(test_decoder test_decoder.cpp)
target_link_libraries(test_decoder PRIVATE atlantic_v5_core)
target_include_directories(test_decoder PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_decoder PRIVATE
    CAPTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../captures")
add_test(NAME test_decoder COMMAND test_decoder)

add_executable(replay replay.cpp)
target_link_libraries(replay PRIVATE replay_lib)

add_executable(test_replay_golden test_replay_golden.cpp)
target_link_libraries(test_replay_golden PRIVATE replay_lib)
target_include_directories(test_replay_golden PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_replay_golden PRIVATE
    CAPTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../captures")
add_test(NAME test_replay_golden COMMAND test_replay_golden)

add_executable(test_relay test_relay.cpp)
target_link_libraries(test_relay PRIVATE atlantic_v5_transport)
target_include_directories(test_relay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_relay PRIVATE
    CAPTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../captures")
add_test(NAME test_relay COMMAND test_relay)

add_executable(test_listener test_listener.cpp)
target_link_libraries(test_listener PRIVATE atlantic_v5_transport)
target_include_directories(test_listener PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_listener PRIVATE
    CAPTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../captures")
add_test(NAME test_listener COMMAND test_listener)
