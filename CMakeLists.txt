cmake_minimum_required(VERSION 3.20)
project(low-latency-matching-engine LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LLME_BUILD_TESTS "Build tests" ON)
option(LLME_BUILD_BENCHMARKS "Build benchmarks" ON)

if(LLME_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(core/matching_engine)

if(LLME_BUILD_BENCHMARKS)
  add_subdirectory(benchmark)
endif()