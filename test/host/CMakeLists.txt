cmake_minimum_required(VERSION 3.16)
project(atlantic_v5_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

# core/ is host-compilable by design (implementation plan, architecture rules):
# no esp-idf, no ESPHome, no Arduino headers, no heap in runtime paths.
add_library(atlantic_v5_core
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/atlantic_v5/core/crc16.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/atlantic_v5/core/frame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/atlantic_v5/core/assembler.cpp)
target_include_directories(atlantic_v5_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/atlantic_v5)

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
