find_package(GTest CONFIG QUIET)

if(NOT GTest_FOUND)
    find_package(GTest QUIET)
endif()

if(NOT GTest_FOUND)
    message(FATAL_ERROR
        "CHIPS_BUILD_TESTS=ON but GTest was not found. "
        "Install it or point CMake at it, e.g. -DCMAKE_PREFIX_PATH=/path/to/gtest")
endif()

include(GoogleTest)

add_library(chips_test_support INTERFACE)
target_include_directories(chips_test_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_compile_features(chips_test_support INTERFACE cxx_std_14)
target_link_libraries(
    chips_test_support
    INTERFACE
        GTest::gtest_main
        chips_sanitizers
)

function(chips_add_test module)
    set(target chips_${module}_test)
    add_executable(${target} "${CMAKE_CURRENT_SOURCE_DIR}/${module}_test.cpp")
    target_link_libraries(
        ${target}
        PRIVATE
            chips_core
            chips_master
            chips_slave
            chips_test_support
    )
    gtest_discover_tests(${target})
endfunction()

chips_add_test(core)
chips_add_test(master)
chips_add_test(slave)
