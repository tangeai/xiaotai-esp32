# SPDX-License-Identifier: MIT
find_package(Python3 REQUIRED COMPONENTS Interpreter)
xiaotai_host_test(board_wt99_parts SOURCES board_test.cc LINK xiaotai_board ${parts}
                  INCLUDE "${generated}" "${XIAOTAI_ROOT}/sdk/parts/xiaotai_part_panel_jd9165/src"
                  "${XIAOTAI_ROOT}/sdk/parts/xiaotai_part_touch_gt911/src"
                  "${XIAOTAI_ROOT}/sdk/parts/xiaotai_part_camera_csi/src"
                  "${XIAOTAI_ROOT}/sdk/parts/xiaotai_part_camera_csi/host")
foreach(symbol xiaotai_bus_io_pin_set xiaotai_bus_io_pwm_setup xiaotai_bus_io_pwm_stop
               xiaotai_bus_io_i2c_open xiaotai_delay_ms xiaotai_mem_has_external xiaotai_mem_is_external)
    target_link_options(board_wt99_parts PRIVATE "LINKER:--wrap=${symbol}")
endforeach()

if(XIAOTAI_HOST_SANITIZE AND xiaotai_ui IN_LIST XIAOTAI_HOST_SELECTED)
    include("${XIAOTAI_ROOT}/tests/sdk/integration/wt99_display_bind_tests.cmake")
endif()
foreach(case IN ITEMS display-success c61 camera control-bus-power)
    add_test(NAME board_wt99_${case} COMMAND board_wt99_parts ${case})
endforeach()
foreach(step RANGE 1 13)
    add_test(NAME board_wt99_display_fail_${step} COMMAND board_wt99_parts display-fail ${step} 0)
endforeach()
foreach(step RANGE 1 8)
    add_test(NAME board_wt99_display_release_fail_${step} COMMAND board_wt99_parts display-fail 13 ${step})
endforeach()
foreach(pin IN ITEMS 0 22 23 26 27 35 53)
    add_test(NAME board_wt99_init_pin_${pin} COMMAND board_wt99_parts init-pin ${pin})
endforeach()
add_test(NAME board_wt99_build_contract COMMAND "${Python3_EXECUTABLE}" -B
         "${CMAKE_CURRENT_SOURCE_DIR}/build_contract.py")
xiaotai_host_role_path(hosted hosted board_wt99_hosted_spi_graph)
if(hosted)
    add_test(NAME board_wt99_hosted_spi_graph COMMAND "${Python3_EXECUTABLE}" -B
             "${CMAKE_CURRENT_SOURCE_DIR}/build_contract.py" "${hosted}" "${CMAKE_C_COMPILER}"
             "$<IF:$<BOOL:${XIAOTAI_HOST_SANITIZE}>,ON,OFF>")
endif()
