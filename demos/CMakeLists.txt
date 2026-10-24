add_executable(
    chips_core_demo
    "${CMAKE_CURRENT_LIST_DIR}/core_demo.c"
)

target_link_libraries(
    chips_core_demo
    PRIVATE
        chips_core
)
