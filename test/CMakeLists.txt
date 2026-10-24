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

add_executable(
    chips_core_test
    "${CMAKE_CURRENT_LIST_DIR}/core_test.cpp"
)

target_link_libraries(
    chips_core_test
    PRIVATE
        chips_core
        GTest::gtest_main
)

target_compile_features(
    chips_core_test
    PRIVATE
        cxx_std_14
)

gtest_discover_tests(chips_core_test)
