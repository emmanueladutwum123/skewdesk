cmake_minimum_required(VERSION 3.16)
project(skewdesk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

option(SD_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(SD_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(SD_ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)
option(SD_BUILD_TESTS "Build the GoogleTest unit test suite" ON)
option(SD_BUILD_BENCHMARKS "Build Google Benchmark microbenchmarks" OFF)

add_library(skewdesk STATIC
  src/black_scholes.cpp
  src/implied_volatility.cpp
  src/parity.cpp
  src/chain.cpp
  src/svi.cpp
  src/surface.cpp
  src/portfolio.cpp
  src/quoting.cpp
  src/simulation.cpp
)

target_include_directories(skewdesk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(skewdesk PUBLIC cxx_std_20)

set(SD_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
if(SD_WARNINGS_AS_ERRORS)
  list(APPEND SD_WARNING_FLAGS -Werror)
endif()
target_compile_options(skewdesk PRIVATE ${SD_WARNING_FLAGS})

if(SD_ENABLE_ASAN)
  target_compile_options(skewdesk PUBLIC -fsanitize=address -fno-omit-frame-pointer -g)
  target_link_options(skewdesk PUBLIC -fsanitize=address)
endif()
if(SD_ENABLE_UBSAN)
  target_compile_options(skewdesk PUBLIC -fsanitize=undefined -fno-omit-frame-pointer -g)
  target_link_options(skewdesk PUBLIC -fsanitize=undefined)
endif()

add_executable(skewdesk_demo src/main.cpp)
target_link_libraries(skewdesk_demo PRIVATE skewdesk)
target_compile_options(skewdesk_demo PRIVATE ${SD_WARNING_FLAGS})

if(SD_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(SD_BUILD_BENCHMARKS)
  include(FetchContent)
  FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.1
  )
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)
endif()

add_subdirectory(bench)
