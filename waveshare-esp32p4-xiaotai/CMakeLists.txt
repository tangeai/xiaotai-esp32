cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.0.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# IDF serializes local overrides as absolute paths. Keep that generated lock in
# build/, while the reviewed dependency versions and hashes stay portable in Git.
include(${CMAKE_CURRENT_LIST_DIR}/tools/dependency_lock.cmake)
xiaotai_prepare_dependency_lock("${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_BINARY_DIR}")
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_BINARY_DIR}/dependencies.lock")
project(xiaotai_esp32p4)

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
         ${PYTHON} -X utf8 ${CMAKE_CURRENT_LIST_DIR}/../lckfb-szpi-esp32s3-tirtc/tools/check_build_policies.py
         --policy i2c_driver_family --nm ${CMAKE_NM}
         --elf $<TARGET_FILE:${CMAKE_PROJECT_NAME}.elf>
 VERBATIM)
set_property(TARGET ${CMAKE_PROJECT_NAME}.elf APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/tools/check_product_image.py"
    "${CMAKE_CURRENT_LIST_DIR}/../lckfb-szpi-esp32s3-tirtc/tools/check_build_policies.py")
