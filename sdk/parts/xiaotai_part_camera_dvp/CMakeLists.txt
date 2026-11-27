# SPDX-License-Identifier: MIT
# XiaoTai parts library: a camera sensor on the parallel interface as a camera
# part (xiaotai/parts/camera_dvp.h).
set(sources src/camera_dvp.c)
if(ESP_PLATFORM)
    set(dvp_product OFF)
    set(native_sources)
    set(native_includes)
    if(NOT CMAKE_BUILD_EARLY_EXPANSION AND XIAOTAI_PORT_V3_PRODUCT)
        set(dvp_product ON)
        set(native_sources ${sources} esp-idf/camera_dvp_io.c)
        set(native_includes src)
    endif()
    idf_component_register(SRCS ${native_sources} INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS ${native_includes}
                           REQUIRES xiaotai_port xiaotai_hal xiaotai_part_bus)
    if(dvp_product)
        # The product must already select the locked managed driver. Do not
        # change requirements during Kconfig expansion or fetch it here.
        idf_build_get_property(build_components BUILD_COMPONENTS)
        if(NOT "espressif__esp32-camera" IN_LIST build_components OR NOT "esp_driver_i2c" IN_LIST build_components)
            message(FATAL_ERROR "Native DVP requires the product's locked esp32-camera and new I2C driver")
        endif()
        idf_component_optional_requires(PRIVATE espressif__esp32-camera esp_driver_i2c)
        if(CONFIG_XIAOTAI_PART_CAMERA_DVP_NATIVE)
            target_compile_definitions(${COMPONENT_LIB} PRIVATE XIAOTAI_DVP_USE_OVERLAY=1)
            include(esp-idf/native_overlay.cmake)
            cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL xiaotai_dvp_enable_native_overlay)
        endif()
    endif()
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port xiaotai_hal xiaotai_part_bus)
add_library(xiaotai_part_camera_dvp STATIC ${sources} host/camera_dvp_io.c)
target_include_directories(xiaotai_part_camera_dvp PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_part_camera_dvp PUBLIC xiaotai_port xiaotai_hal xiaotai_part_bus)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
