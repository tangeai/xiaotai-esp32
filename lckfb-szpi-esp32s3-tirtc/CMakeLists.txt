cmake_minimum_required(VERSION 3.16)

# Diagnostic firmware identity. Keep this explicit so ESP-IDF embeds the same
# human-readable version in the app descriptor and bootloader startup output.
set(PROJECT_VER "1.2.0")

# 生成器把 SDK 复制到 third_party/tirtc，使生成工程可以离开本仓库独立构建。
# 高级用户仍可通过 -DTIRTC_SDK_DIR=<path> 指向另一份兼容 SDK。
set(TIRTC_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/third_party/tirtc"
    CACHE PATH "TiRTC ESP32-S3 SDK root")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lckfb_szpi_esp32s3_tirtc)

include(cmake/tflm_conv_channels.cmake)
starter_enable_tflm_conv_channels()

include(cmake/mqtt_psram_buffers.cmake)
starter_enable_mqtt_psram_buffers()

include(cmake/lvgl_psram_pool.cmake)
starter_enable_lvgl_psram_pool()

# This Kconfig symbol is hidden when trace support is off, so confgen omits its
# explicit "not set" line even though the value is correct. Device Kit Doctor
# requires all ABI contract keys to remain explicit in the generated sdkconfig.
set(tirtc_stats_marker "# CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS is not set")
file(READ "${CMAKE_CURRENT_LIST_DIR}/sdkconfig" tirtc_sdkconfig_text)
string(FIND "${tirtc_sdkconfig_text}" "${tirtc_stats_marker}" tirtc_stats_marker_pos)
if(tirtc_stats_marker_pos EQUAL -1)
    file(APPEND "${CMAKE_CURRENT_LIST_DIR}/sdkconfig" "\n${tirtc_stats_marker}\n")
endif()

# Source/ELF gates must run with IDF's host Python on both Windows and Unix.
# An unqualified bash on Windows can launch WSL with untranslated C:/ paths.
idf_build_get_property(starter_policy_python PYTHON)
add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}.elf
    POST_BUILD
    COMMAND "${starter_policy_python}" -X utf8
            "${CMAKE_CURRENT_LIST_DIR}/tools/check_build_policies.py"
            --root "${CMAKE_CURRENT_LIST_DIR}"
            --elf "$<TARGET_FILE:${CMAKE_PROJECT_NAME}.elf>"
            --nm "${CMAKE_NM}"
            --objdump "${CMAKE_OBJDUMP}"
    VERBATIM
)

# A changed policy must be re-run even if no firmware source changed.
set_property(TARGET ${CMAKE_PROJECT_NAME}.elf APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/tools/check_build_policies.py")
