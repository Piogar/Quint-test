cmake_minimum_required(VERSION 3.20)
project(stats_framework LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------- library (src/)
add_library(stats_framework
    src/StatsHolder.cpp
    src/StatsFlusher.cpp
    src/StatsCollector.cpp
    src/StatsOrchestrator.cpp
)
target_include_directories(stats_framework PUBLIC src)
target_compile_options(stats_framework PRIVATE -Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------- tests (tests/)
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest nlohmann_json)

enable_testing()

# Unit tests: every branch of every operation
add_executable(stats_unit_tests tests/unit_tests.cpp)
target_include_directories(stats_unit_tests PRIVATE tests)
target_link_libraries(stats_unit_tests PRIVATE stats_framework GTest::gtest_main)
target_compile_options(stats_unit_tests PRIVATE -Wall -Wextra)

# MBT harness: replays the ITF traces in traces/ (scripts/gen_traces.sh)
add_executable(stats_mbt_tests tests/mbt_test.cpp)
target_include_directories(stats_mbt_tests PRIVATE tests)
target_link_libraries(stats_mbt_tests PRIVATE stats_framework GTest::gtest_main nlohmann_json::nlohmann_json)
target_compile_definitions(stats_mbt_tests PRIVATE TRACES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/traces")
target_compile_options(stats_mbt_tests PRIVATE -Wall -Wextra)

include(GoogleTest)
gtest_discover_tests(stats_unit_tests)
gtest_discover_tests(stats_mbt_tests)
