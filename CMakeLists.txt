cmake_minimum_required(VERSION 3.21)
project(chips VERSION 0.1.0 LANGUAGES C)

option(CHIPS_BUILD_TESTS "Build CHIPS unit tests (requires GTest)" OFF)
option(CHIPS_BUILD_DEMOS "Build CHIPS demos" OFF)

if(CHIPS_BUILD_TESTS)
    set(_chips_default_sanitizer asan)
else()
    set(_chips_default_sanitizer none)
endif()

set(CHIPS_SANITIZER "${_chips_default_sanitizer}" CACHE STRING "Sanitizer for all CHIPS targets: asan, none")
set_property(CACHE CHIPS_SANITIZER PROPERTY STRINGS asan none)
if(NOT CHIPS_SANITIZER MATCHES "^(asan|none)$")
    message(FATAL_ERROR "CHIPS_SANITIZER must be 'asan' or 'none', got '${CHIPS_SANITIZER}'")
endif()

add_library(chips_options INTERFACE)
target_compile_options(
    chips_options
    INTERFACE
        "$<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Werror=return-type>"
        "$<$<C_COMPILER_ID:MSVC>:/W4>"
)

add_library(chips_sanitizers INTERFACE)
if(CHIPS_SANITIZER STREQUAL "asan")
    if(MSVC)
        target_compile_options(chips_sanitizers INTERFACE /fsanitize=address /Zi)
    else()
        target_compile_options(
            chips_sanitizers
            INTERFACE
                -fsanitize=address,undefined
                -fno-sanitize-recover=all
                -fno-omit-frame-pointer
                -g
        )
        target_link_options(chips_sanitizers INTERFACE -fsanitize=address,undefined)
    endif()
endif()

function(chips_add_library name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;DEPENDS")
    add_library(${name} ${ARG_SOURCES})
    target_link_libraries(
        ${name}
        PUBLIC
            chips_sanitizers
            ${ARG_DEPENDS}
        PRIVATE
            chips_options
    )
endfunction()

chips_add_library(chips_core SOURCES src/core.c)
target_include_directories(chips_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
chips_add_library(chips_master SOURCES src/master.c DEPENDS chips_core)
chips_add_library(chips_slave SOURCES src/slave.c DEPENDS chips_core)

if(CHIPS_BUILD_TESTS)
    enable_language(CXX)
    enable_testing()
    add_subdirectory(test)
endif()

if(CHIPS_BUILD_DEMOS)
    add_subdirectory(demos)
endif()
