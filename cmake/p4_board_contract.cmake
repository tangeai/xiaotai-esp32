# Apply only reviewed patches to the selected dependency cache, never the IDF tree.
function(xiaotai_hosted_patch filename)
    idf_component_get_property(hosted_dir espressif__esp_hosted COMPONENT_DIR)
    get_filename_component(cache_root "${hosted_dir}" DIRECTORY)
    find_package(Git REQUIRED)
    set(patch "${XIAOTAI_SELECTED_BOARD}/build-support/tools/patches/${filename}")
    set(apply "${CMAKE_COMMAND}" -E env --unset=GIT_DIR --unset=GIT_WORK_TREE
        --unset=GIT_INDEX_FILE "GIT_CEILING_DIRECTORIES=${cache_root}" "${GIT_EXECUTABLE}" apply)
    if(NOT EXISTS "${hosted_dir}/host/features/eh_host_feat_system/src/eh_host_sys.c")
        message(FATAL_ERROR "Selected ESP-Hosted component is incomplete")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${patch}")
    execute_process(COMMAND ${apply} --reverse --check --ignore-space-change "${patch}"
        WORKING_DIRECTORY "${hosted_dir}" RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
    if(NOT applied EQUAL 0)
        execute_process(COMMAND ${apply} --check --ignore-space-change "${patch}"
            WORKING_DIRECTORY "${hosted_dir}" RESULT_VARIABLE clean ERROR_VARIABLE detail)
        if(NOT clean EQUAL 0)
            message(FATAL_ERROR "Hosted patch does not match locked input: ${filename}: ${detail}")
        endif()
        execute_process(COMMAND ${apply} --ignore-space-change "${patch}"
            WORKING_DIRECTORY "${hosted_dir}" COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND ${apply} --reverse --check --ignore-space-change "${patch}"
            WORKING_DIRECTORY "${hosted_dir}" COMMAND_ERROR_IS_FATAL ANY)
    endif()
endfunction()

if(CONFIG_ESP32P4_SELECTS_REV_LESS_V3 OR
   NOT CONFIG_ESP32P4_REV_MIN_FULL EQUAL 301 OR
   NOT CONFIG_ESP32P4_REV_MAX_FULL EQUAL 399 OR
   NOT CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ EQUAL 400 OR CONFIG_HAL_WDT_USE_ROM_IMPL)
    message(FATAL_ERROR "P4 profile requires rev3.1-3.99, 400 MHz and non-ROM WDT; no rev1 fallback")
endif()
xiaotai_hosted_patch(esp-hosted-3.0.7-app-desc.patch)
if(XIAOTAI_PROFILE_ID STREQUAL "p4")
    if(NOT CONFIG_ESP_HOSTED_HOST_TRANSPORT_BUS_SDIO OR
       NOT DEFINED CONFIG_ESP_HOSTED_HOST_SDIO_CLK_KHZ OR
       CONFIG_ESP_HOSTED_HOST_SDIO_CLK_KHZ GREATER 40000)
        message(FATAL_ERROR "P4/C6 requires explicit SDR SDIO clock <= 40000 kHz")
    endif()
    xiaotai_hosted_patch(esp-hosted-3.0.7-p4-sdio-rx-bounce.patch)
elseif(XIAOTAI_PROFILE_ID STREQUAL "wt99")
    idf_build_get_property(PYTHON PYTHON)
    include("${XIAOTAI_SELECTED_BOARD}/build-support/check_board_config.cmake")
else()
    message(FATAL_ERROR "Unknown P4 board contract")
endif()
set(check "${XIAOTAI_SELECTED_BOARD}/build-support/check_firmware.cmake")
add_custom_command(TARGET ${CMAKE_PROJECT_NAME}.elf POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -DCROSS_NM=${CMAKE_NM}
        -DELF=$<TARGET_FILE:${CMAKE_PROJECT_NAME}.elf>
        -DMAP=${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.map
        -DCONFIG=${CMAKE_BINARY_DIR}/config/sdkconfig.json -P "${check}"
    VERBATIM)
set_property(TARGET ${CMAKE_PROJECT_NAME}.elf APPEND PROPERTY LINK_DEPENDS "${check}")
