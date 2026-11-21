# SPDX-License-Identifier: MIT
# XiaoTai parts library: a camera sensor on MIPI CSI behind the image signal
# processor as a camera part (xiaotai/parts/camera_csi.h).
set(sources src/camera_csi.c)
if(ESP_PLATFORM)
    if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
        set(dma_component esp_driver_dma)
    else()
        set(dma_component esp_hw_support)
    endif()
    idf_component_register(SRCS ${sources} esp-idf/camera_csi_io.c
                           INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src"
                           REQUIRES xiaotai_port xiaotai_hal xiaotai_part_bus
                           PRIV_REQUIRES esp_video esp_driver_i2c esp_mm ${dma_component})
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port xiaotai_hal xiaotai_part_bus)
add_library(xiaotai_part_camera_csi STATIC ${sources} host/camera_csi_io.c)
target_include_directories(xiaotai_part_camera_csi PUBLIC include PRIVATE src host)
target_link_libraries(xiaotai_part_camera_csi PUBLIC xiaotai_port xiaotai_hal xiaotai_part_bus)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
