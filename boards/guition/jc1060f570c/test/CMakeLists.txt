# SPDX-License-Identifier: MIT
find_package(Python3 REQUIRED COMPONENTS Interpreter)
xiaotai_host_test(board_jc1060f570c_parts SOURCES board_test.cc LINK xiaotai_board ${parts}
                  INCLUDE "${generated}")
foreach(symbol xiaotai_mem_has_external xiaotai_i2c_create xiaotai_line_gpio_output xiaotai_level_pwm
               xiaotai_es8389_create xiaotai_audio_device_create xiaotai_panel_rgb_create
               xiaotai_touch_gt911_create xiaotai_display_create)
    target_link_options(board_jc1060f570c_parts PRIVATE "LINKER:--wrap=${symbol}")
endforeach()
add_test(NAME board_jc1060f570c_manifest COMMAND "${Python3_EXECUTABLE}" -B
         "${CMAKE_CURRENT_SOURCE_DIR}/compare_manifest.py" "${manifest}" $<TARGET_FILE:board_jc1060f570c_parts>)
add_test(NAME board_jc1060f570c_wiring COMMAND board_jc1060f570c_parts wiring)
