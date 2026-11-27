# SPDX-License-Identifier: MIT
# XiaoTai video engine (optional; for products that send or show pictures): the
# camera uplink (JPEG, H.264), the picture a peer sends (decode, convert,
# present) and the governor that keeps the uplink inside what the link carries.
# The portable core (src/) knows sessions, generations, pacing, key frames,
# pools and geometry; codecs and accelerators are backends registered from
# esp-idf/ (hardware H.264 and JPEG, the pixel accelerator, the software H.264
# decoder, JPEG encoder and JPEG decoder) or host/ (none: tests register fakes).
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
    # What each chip brings: requirements are collected before its SOC
    # capabilities and the build options can be read, so the chips are named
    # here and nowhere else. IDF 5.5 has no H.264 SOC macro.
    set(chips_h264_decoder esp32p4 esp32s31)    # fast enough for the software decoder
    set(chips_h264_encoder esp32p4)
    set(chips_jpeg_decoder esp32p4 esp32s31)
    set(chips_pixel_accelerator esp32p4)
    set(definitions)
    if(IDF_TARGET IN_LIST chips_h264_decoder)
        list(APPEND native esp-idf/decoder_h264_sw.c)
        list(APPEND native_requires espressif__esp_h264)
        list(APPEND definitions XIAOTAI_VIDEO_H264_DECODER=1)
    endif()
    if(IDF_TARGET IN_LIST chips_h264_encoder)
        list(APPEND native esp-idf/encoder_h264_hw.c)
        list(APPEND native_requires espressif__esp_h264 esp_mm)
        list(APPEND definitions XIAOTAI_VIDEO_H264_ENCODER=1)
    endif()
    if(IDF_TARGET IN_LIST chips_jpeg_decoder)
        list(APPEND native esp-idf/decoder_jpeg_hw.c)
        list(APPEND native_requires esp_driver_jpeg)
        list(APPEND definitions XIAOTAI_VIDEO_JPEG_DECODER=1)
    endif()
    if(IDF_TARGET IN_LIST chips_pixel_accelerator)
        list(APPEND native esp-idf/transform_ppa.c)
        list(APPEND native_requires esp_driver_ppa esp_mm)
        list(APPEND definitions XIAOTAI_VIDEO_PIXEL_ACCELERATOR=1)
    endif()
    if(CONFIG_XIAOTAI_VIDEO_JPEG_SOFTWARE_DECODER)
        list(APPEND native esp-idf/decoder_jpeg_sw.c)
    endif()
    list(REMOVE_DUPLICATES native_requires)
    idf_component_register(SRCS ${sources} ${native}
                           INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src"
                           REQUIRES ${requires} PRIV_REQUIRES ${native_requires})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${definitions})
    # Every pixel of every picture goes through these.
    set_source_files_properties(src/transform_sw.c src/paint.c PROPERTIES COMPILE_OPTIONS "-O2")
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
