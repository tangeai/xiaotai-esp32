cmake_minimum_required(VERSION 3.16)

# Dependencies are pinned. The optional update probe in Component Manager 2.4.9
# misreads LVGL 8 optional Kconfig symbols on a normal Windows reconfigure.
# Keep dependency resolution enabled; skip only that probe for this project.
if(NOT DEFINED ENV{IDF_COMPONENT_CHECK_NEW_VERSION})
    set(ENV{IDF_COMPONENT_CHECK_NEW_VERSION} "0")
endif()

set(PROJECT_VER "1.5.0")
# This Kconfig is loaded through starter_voice's rsource. Track it explicitly
# so incremental Ninja builds regenerate sdkconfig.h when its options change.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/main/Kconfig.xiaotai")
# Wi-Fi Remote 0.14 includes Kconfig by this major.minor environment value.
# Derive it from the selected IDF even on Ninja/CMake-triggered reconfigure;
# a missing value silently drops C6/SDIO symbols before Hosted compilation.
include($ENV{IDF_PATH}/tools/cmake/version.cmake)
set(ENV{ESP_IDF_VERSION} "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# IDF serializes local overrides as absolute paths. Keep that generated lock in
# build/, while the reviewed dependency versions and hashes stay portable in Git.
include(${CMAKE_CURRENT_LIST_DIR}/tools/dependency_lock.cmake)
xiaotai_prepare_dependency_lock("${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_BINARY_DIR}")
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_BINARY_DIR}/dependencies.lock")
project(xiaotai_esp32p4)

# This branch produces firmware for v3.x silicon only. Never reuse a cached
# rev1 sdkconfig or flash its output to a rev1 board.
if(CONFIG_ESP32P4_SELECTS_REV_LESS_V3 OR
   NOT CONFIG_ESP32P4_REV_MIN_FULL EQUAL 301 OR
   NOT CONFIG_ESP32P4_REV_MAX_FULL EQUAL 399 OR
   NOT CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ EQUAL 400 OR
   CONFIG_HAL_WDT_USE_ROM_IMPL)
    message(FATAL_ERROR
        "P4 rev3.2 validation requires rev3.1-3.99, 400 MHz and non-ROM WDT. "
        "Current sdkconfig: min=${CONFIG_ESP32P4_REV_MIN_FULL}, "
        "max=${CONFIG_ESP32P4_REV_MAX_FULL}, "
        "cpu=${CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ}, "
        "rom_wdt=${CONFIG_HAL_WDT_USE_ROM_IMPL}. "
        "A previous branch's ignored sdkconfig/build cache cannot be reused; "
        "preserve it separately and regenerate this branch's configuration.")
endif()

# This Hosted port selects SDR flags. IDF 5.5.4 reserves 50000 for DDR50 and
# rejects it before probing the C6. Check the effective config, not defaults.
if(CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE AND
   CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ GREATER 40000)
 message(FATAL_ERROR
   "P4/C6 Hosted SDR requires CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ <= 40000. "
   "ESP-IDF interprets 50000 as DDR50; do not enable DDR to bypass this check.")
endif()

add_custom_command(TARGET ${CMAKE_PROJECT_NAME}.elf POST_BUILD
 COMMAND ${CMAKE_COMMAND}
         -DCROSS_NM=${CMAKE_NM}
         -DELF=$<TARGET_FILE:${CMAKE_PROJECT_NAME}.elf>
         -DMAP=${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.map
         -DCONFIG=${CMAKE_BINARY_DIR}/config/sdkconfig.json
         -P ${CMAKE_CURRENT_LIST_DIR}/tools/check_firmware.cmake
 VERBATIM)
set_property(TARGET ${CMAKE_PROJECT_NAME}.elf APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/tools/check_firmware.cmake")
