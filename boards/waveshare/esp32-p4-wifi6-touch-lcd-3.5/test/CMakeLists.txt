# SPDX-License-Identifier: MIT
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(part_root "${CMAKE_CURRENT_LIST_DIR}/../../../../sdk/parts")
xiaotai_host_test(board_waveshare_p4 SOURCES board_test.cc LINK xiaotai_board ${parts}
    INCLUDE "${generated}"
    "${part_root}/xiaotai_part_panel_spi/host"
    "${part_root}/xiaotai_part_camera_csi/host"
    "${part_root}/xiaotai_part_camera_csi/src"
    "${part_root}/xiaotai_part_audio/host"
    "${part_root}/xiaotai_part_audio/src")
foreach(symbol
        xiaotai_i2c_create xiaotai_line_gpio_output xiaotai_level_pwm xiaotai_line_gpio_input
        xiaotai_es8311_create xiaotai_audio_device_create xiaotai_panel_spi_create
        xiaotai_touch_ft5x06_create xiaotai_display_create xiaotai_button_create
        xiaotai_camera_csi_create xiaotai_coprocessor_create
        xiaotai_mem_has_external xiaotai_mem_is_external)
    target_link_options(board_waveshare_p4 PRIVATE "LINKER:--wrap=${symbol}")
endforeach()
foreach(step RANGE 1 13)
    add_test(NAME board_waveshare_p4_init_failure_${step} COMMAND board_waveshare_p4 ${step})
endforeach()
foreach(mode display camera audio update unknown)
    add_test(NAME board_waveshare_p4_${mode} COMMAND board_waveshare_p4 ${mode})
endforeach()
add_test(NAME board_waveshare_p4_manifest COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_LIST_DIR}/check_package.py" "${CMAKE_COMMAND}"
    "${manifest}" $<TARGET_FILE:board_waveshare_p4> "${XIAOTAI_ROOT}")
