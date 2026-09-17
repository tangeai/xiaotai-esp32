cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.3.0")
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

include(${CMAKE_CURRENT_LIST_DIR}/components/starter_voice/conv_channels.cmake)
idf_component_get_property(tflm_lib espressif__esp-tflite-micro COMPONENT_LIB)
idf_component_get_property(tflm_dir espressif__esp-tflite-micro COMPONENT_DIR)
xiaotai_fix_conv_channels(${tflm_lib} "${tflm_dir}"
    "${CMAKE_BINARY_DIR}/p4_tflm")

# This Hosted port selects SDR flags. IDF 5.5.4 reserves 50000 for DDR50 and
# rejects it before probing the C6. Check the effective config, not defaults.
if(CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE AND
   CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ GREATER 40000)
 message(FATAL_ERROR
   "P4/C6 Hosted SDR requires CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ <= 40000. "
   "ESP-IDF interprets 50000 as DDR50; do not enable DDR to bypass this check.")
endif()

add_custom_command(TARGET ${CMAKE_PROJECT_NAME}.elf POST_BUILD
 COMMAND ${CMAKE_COMMAND} -E env CROSS_NM=${CMAKE_NM}
            ${PYTHON} ${CMAKE_CURRENT_LIST_DIR}/tools/check_product_image.py
            $<TARGET_FILE:${CMAKE_PROJECT_NAME}.elf>
            --config ${CMAKE_BINARY_DIR}/config/sdkconfig.json
 COMMAND ${CMAKE_COMMAND} -E env CROSS_NM=${CMAKE_NM}
         ${PYTHON} -X utf8 ${CMAKE_CURRENT_LIST_DIR}/tools/check_i2c_driver_family.py
         --nm ${CMAKE_NM}
         --elf $<TARGET_FILE:${CMAKE_PROJECT_NAME}.elf>
 VERBATIM)
set_property(TARGET ${CMAKE_PROJECT_NAME}.elf APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/tools/check_product_image.py"
    "${CMAKE_CURRENT_LIST_DIR}/tools/check_i2c_driver_family.py")
