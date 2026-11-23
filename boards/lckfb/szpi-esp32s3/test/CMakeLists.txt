# SPDX-License-Identifier: MIT
find_package(Python3 REQUIRED COMPONENTS Interpreter)
xiaotai_host_test(board_szpi_parts SOURCES board_test.cc LINK xiaotai_board ${parts}
                  INCLUDE "${generated}"
                          "${XIAOTAI_ROOT}/sdk/parts/xiaotai_part_audio/host"
                          "${XIAOTAI_ROOT}/sdk/parts/xiaotai_part_audio/src"
                          "${XIAOTAI_ROOT}/sdk/parts/xiaotai_part_panel_spi/host"
                          "${XIAOTAI_ROOT}/sdk/parts/xiaotai_part_camera_dvp/host"
                          "${XIAOTAI_ROOT}/sdk/parts/xiaotai_part_camera_dvp/src")
foreach(symbol xiaotai_mem_has_external xiaotai_i2c_create xiaotai_pca9557_create xiaotai_pca9557_line
               xiaotai_line_gpio_output xiaotai_line_gpio_input xiaotai_es7210_create xiaotai_es8311_create
               xiaotai_audio_device_create xiaotai_panel_spi_create xiaotai_touch_ft5x06_create
               xiaotai_display_create xiaotai_button_create xiaotai_camera_dvp_create)
    target_link_options(board_szpi_parts PRIVATE "LINKER:--wrap=${symbol}")
endforeach()
add_test(NAME board_szpi_manifest COMMAND "${Python3_EXECUTABLE}" -B
         "${CMAKE_CURRENT_SOURCE_DIR}/compare_manifest.py" "${manifest}" $<TARGET_FILE:board_szpi_parts>)
foreach(step RANGE 1 15)
    add_test(NAME board_szpi_failure_${step} COMMAND board_szpi_parts ${step})
endforeach()
foreach(transfer RANGE 1 3)
    add_test(NAME board_szpi_i2c_failure_${transfer} COMMAND board_szpi_parts -${transfer})
endforeach()
foreach(case audio display camera button shared cleanup-failure)
    add_test(NAME board_szpi_${case} COMMAND board_szpi_parts ${case})
endforeach()
