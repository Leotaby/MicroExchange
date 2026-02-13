cmake_minimum_required(VERSION 3.20)
project(MicroExchange
    VERSION 1.0.0
    LANGUAGES CXX
    DESCRIPTION "Exchange-grade CLOB matching engine + microstructure analytics"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# keeping it simple, tried -march=native but it broke on a colleague's M1
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer")

add_compile_options(-Wall -Wextra -Wpedantic)

# all headers visible globally
include_directories(
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/md/include
    ${CMAKE_SOURCE_DIR}/sim/include
    ${CMAKE_SOURCE_DIR}/analytics/include
)

# main simulation binary
add_executable(micro_exchange src/main.cpp)

# tests
add_executable(test_invariants core/tests/test_invariants.cpp)

# benchmarks
add_executable(bench_throughput bench/bench_throughput.cpp)

install(TARGETS micro_exchange test_invariants bench_throughput
    RUNTIME DESTINATION bin
)
