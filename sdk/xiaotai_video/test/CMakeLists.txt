# SPDX-License-Identifier: MIT
# Host tests of the video engine. Each test compiles the engine's sources
# itself, with the shared fakes (fakes.[ch]): the hardware abstraction is
# replaced (INCLUDE_ONLY), the port, framework and event log are real. Port
# calls are recorded by linking with --wrap: allocations by memory kind (the
# host memory port counts all kinds together), task creations with their
# configuration, and the clock. A test compiled with other build options gets
# them as compile definitions, so the engine's sources see them too.
set(video_engine_sources ../src/video.c ../src/registry.c ../src/geometry.c ../src/transform_sw.c ../src/paint.c
                         ../src/viewport.c ../src/session.c ../src/governor.c ../src/pacing.c
                         ../src/uplink.c ../src/uplink_jpeg.c ../src/uplink_h264.c ../src/reserve.c ../src/overlay.c
                         ../src/downlink.c ../src/h264_au.c ../src/convert.c ../src/present.c ../src/playout.c
                         ../src/video_at.c ../src/capture.c ../src/probe.c
                         ../host/video_port.c)

function(video_test name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;DEFINES")
    xiaotai_host_test(${name}
        SOURCES ${arg_SOURCES} fakes.c ${video_engine_sources}
        INCLUDE ../src
        INCLUDE_ONLY xiaotai_video
        LINK xiaotai_port xiaotai_framework xiaotai_eventlog xiaotai_at)
    target_link_options(${name} PRIVATE
        "LINKER:--wrap=xiaotai_mem_alloc,--wrap=xiaotai_mem_calloc,--wrap=xiaotai_mem_aligned"
        "LINKER:--wrap=xiaotai_task_create,--wrap=xiaotai_time_us")
    if(arg_DEFINES)
        target_compile_definitions(${name} PRIVATE ${arg_DEFINES})
    endif()
endfunction()

# W1: registries, requirements, lifecycle.
video_test(video_registry SOURCES video_registry_test.c DEFINES VIDEO_TEST_BACKENDS_MAX=8)
# The registry capacity is the build option, not a constant of the sources.
video_test(video_registry_capacity SOURCES video_registry_test.c
           DEFINES CONFIG_XIAOTAI_VIDEO_BACKENDS_MAX=2 VIDEO_TEST_BACKENDS_MAX=2)
video_test(video_require SOURCES video_require_test.c)

# W2: crop/place plans, actual pixels, clipped presentation.
xiaotai_host_test(video_geometry SOURCES video_geometry_test.c LINK xiaotai_video INCLUDE ../src)
xiaotai_host_test(video_transform_sw SOURCES video_transform_sw_test.c LINK xiaotai_video INCLUDE ../src)
xiaotai_host_test(video_paint SOURCES video_paint_test.c LINK xiaotai_video INCLUDE ../src)

# Pure W3 logic, compiled and linked from the shared production target.
xiaotai_host_test(video_governor SOURCES video_governor_test.c LINK xiaotai_video)
xiaotai_host_test(video_pacing SOURCES video_pacing_test.c LINK xiaotai_video INCLUDE ../src)

xiaotai_host_test(video_link SOURCES video_link_test.c ../src/video_link.c
                  INCLUDE_ONLY xiaotai_video xiaotai_transport)
video_test(video_session SOURCES video_session_test.c)
target_link_options(video_session PRIVATE
    "LINKER:--wrap=video_session_task,--wrap=video_uplink_task,--wrap=video_uplink_prepare_boot")
video_test(video_uplink_jpeg SOURCES video_uplink_jpeg_test.c)
video_test(video_reserve SOURCES video_reserve_test.c)
video_test(video_uplink_h264 SOURCES video_uplink_h264_test.c)
xiaotai_host_test(video_overlay SOURCES video_overlay_test.c LINK xiaotai_video INCLUDE ../src)
xiaotai_host_test(video_h264_au SOURCES video_h264_au_test.c LINK xiaotai_video INCLUDE ../src)
video_test(video_downlink SOURCES video_downlink_test.c)
target_link_options(video_downlink PRIVATE
    "LINKER:--wrap=video_session_task,--wrap=video_convert_task,--wrap=video_convert_kick,--wrap=video_session_downlink_buffering")
video_test(video_on_demand SOURCES video_on_demand_test.c)
target_link_options(video_on_demand PRIVATE
    "LINKER:--wrap=video_session_task,--wrap=video_convert_task,--wrap=video_convert_kick,--wrap=video_session_downlink_buffering"
    "LINKER:--wrap=xiaotai_mem_stats")
video_test(video_present SOURCES video_present_test.c)
xiaotai_host_test(video_jpeg_decoder SOURCES video_jpeg_decoder_test.c ../esp-idf/decoder_jpeg_sw.c
                  INCLUDE jpeg_library INCLUDE_ONLY xiaotai_video LINK xiaotai_port)
xiaotai_host_test(video_playout SOURCES video_playout_test.c LINK xiaotai_video INCLUDE ../src)
video_test(video_convert SOURCES video_convert_test.c)
xiaotai_host_test(video_capture SOURCES video_capture_test.c ../src/capture.c ../src/h264_au.c
                  INCLUDE ../src INCLUDE_ONLY xiaotai_video LINK xiaotai_port)
target_link_options(video_capture PRIVATE "LINKER:--wrap=xiaotai_mutex_lock,--wrap=xiaotai_mem_alloc")
video_test(video_probe SOURCES video_probe_test.c)
target_link_options(video_probe PRIVATE "LINKER:--wrap=xiaotai_at_notifyf")
video_test(video_at SOURCES video_at_test.c)

if(DEFINED ENV{XIAOTAI_VIDEO_NATIVE_TEST_IDF})
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(native_idfs "$ENV{XIAOTAI_VIDEO_NATIVE_TEST_IDF}")
    foreach(idf IN LISTS native_idfs)
        list(APPEND native_args --idf "${idf}")
    endforeach()
    foreach(backend IN ITEMS h264 ppa decoders)
        add_test(NAME video_${backend}_native
                 COMMAND "${Python3_EXECUTABLE}" -B
                         "${CMAKE_CURRENT_SOURCE_DIR}/video_${backend}_native_test.py" ${native_args})
        set_tests_properties(video_${backend}_native PROPERTIES LABELS xiaotai_video TIMEOUT 120)
    endforeach()
endif()
