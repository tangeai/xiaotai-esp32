# SPDX-License-Identifier: MIT
# XiaoTai video engine (optional; only products with a camera select it): the
# camera uplink (JPEG, H.264), the picture a peer sends (decode, convert,
# present) and the governor that keeps the uplink inside what the link carries.
# The portable core (src/) knows sessions, generations, pacing, key frames,
# pools and geometry; codecs and accelerators are backends registered from
# esp-idf/ (hardware H.264 and JPEG, the pixel accelerator, the software H.264
# decoder and JPEG encoder) or host/ (none: tests register fakes).
#
# Native files are isolated from the production host graph.
set(sources src/video.c src/registry.c src/governor.c src/pacing.c
            src/geometry.c src/transform_sw.c src/paint.c src/viewport.c src/session.c src/video_link.c
            src/uplink.c src/uplink_jpeg.c src/uplink_h264.c src/reserve.c src/overlay.c
            src/downlink.c src/h264_au.c src/convert.c src/present.c src/playout.c
            src/video_at.c src/capture.c src/probe.c)
set(requires xiaotai_port xiaotai_framework xiaotai_hal xiaotai_transport xiaotai_at xiaotai_eventlog)
if(ESP_PLATFORM)
    set(native esp-idf/video_port.c esp-idf/encoder_jpeg_sw.c)
    set(native_requires soc)
    # The locked local encoder has a target-specific archive and is only
    # linked where that backend exists. IDF 5.5 has no H.264 SOC macro.
    if(IDF_TARGET STREQUAL "esp32p4")
        list(APPEND native esp-idf/encoder_h264_hw.c esp-idf/transform_ppa.c
                           esp-idf/decoder_h264_sw.c esp-idf/decoder_jpeg_hw.c)
        list(APPEND native_requires espressif__esp_h264 esp_driver_ppa esp_mm esp_driver_jpeg)
    endif()
    idf_component_register(SRCS ${sources} ${native}
                           INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src"
                           REQUIRES ${requires} PRIV_REQUIRES ${native_requires})
    if(IDF_TARGET STREQUAL "esp32p4")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE XIAOTAI_VIDEO_NATIVE_H264=1)
    endif()
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
add_library(xiaotai_video STATIC ${sources} host/video_port.c)
target_include_directories(xiaotai_video PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_video PUBLIC ${requires})
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
