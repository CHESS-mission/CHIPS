cmake_minimum_required(VERSION 3.21)
project(chips VERSION 0.1.0 LANGUAGES C)

option(CHIPS_BUILD_TESTS "Build CHIPS unit tests (requires GTest)" OFF)
option(CHIPS_BUILD_DEMOS "Build CHIPS demos" OFF)

add_library(
    chips_core
    "${CMAKE_CURRENT_LIST_DIR}/src/core.c"
)

target_include_directories(
    chips_core
    PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/include"
)

if(CHIPS_BUILD_TESTS)
    enable_language(CXX)
    enable_testing()
    add_subdirectory(test)
endif()

if(CHIPS_BUILD_DEMOS)
    add_subdirectory(demos)
endif()
