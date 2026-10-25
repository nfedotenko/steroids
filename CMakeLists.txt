cmake_minimum_required(VERSION 3.22)

project(steroids LANGUAGES CXX)

add_library(steroids INTERFACE)
add_library(steroids::steroids ALIAS steroids)

target_include_directories(steroids INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_features(steroids INTERFACE cxx_std_20)

option(STEROIDS_BUILD_TESTS "Build tests" OFF)
option(STEROIDS_BUILD_BENCH "Build benchmarks" OFF)

if(STEROIDS_BUILD_TESTS OR (BUILD_TESTING AND PROJECT_IS_TOP_LEVEL))
  enable_testing()
  add_subdirectory(tests)
endif()

if(STEROIDS_BUILD_BENCH)
  add_subdirectory(bench)
endif()
