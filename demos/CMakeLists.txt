add_executable(
    chips_core_demo
    "${CMAKE_CURRENT_LIST_DIR}/core_demo.c"
)

target_link_libraries(
    chips_core_demo
    PRIVATE
        chips_core
)

add_executable(
    chips_slave_demo
    "${CMAKE_CURRENT_LIST_DIR}/slave_demo.c"
)

target_link_libraries(
    chips_slave_demo
    PRIVATE
        chips_slave
)

add_executable(
    chips_oracle_demo
    "${CMAKE_CURRENT_LIST_DIR}/oracle_demo.c"
)

target_link_libraries(
    chips_oracle_demo
    PRIVATE
        chips_master
        chips_slave
)
