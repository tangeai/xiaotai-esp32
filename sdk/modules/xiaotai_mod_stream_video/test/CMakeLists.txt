# SPDX-License-Identifier: MIT
set(stream_writer "${CMAKE_CURRENT_SOURCE_DIR}/../../xiaotai_mod_stream/src/stream_profile.c")
set(case 0)
foreach(mode offer session present profile)
    xiaotai_host_test(mod_stream_video_${mode} SOURCES stream_video_test.c ${stream_writer}
        LINK xiaotai_mod_stream_video)
    target_compile_definitions(mod_stream_video_${mode} PRIVATE SV_TEST_CASE=${case} SV_TEST_UI=1)
    math(EXPR case "${case} + 1")
endforeach()
xiaotai_host_test(mod_stream_video_no_ui SOURCES stream_video_test.c ${stream_writer}
    ../src/stream_video.c ../src/stream_video_profile.c
    LINK xiaotai_at INCLUDE_ONLY xiaotai_mod_stream_video)
target_compile_definitions(mod_stream_video_no_ui PRIVATE SV_TEST_CASE=2 SV_TEST_UI=0)
