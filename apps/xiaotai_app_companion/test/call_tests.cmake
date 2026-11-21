# SPDX-License-Identifier: MIT
if(xiaotai_mod_call IN_LIST selected AND xiaotai_mod_wechat IN_LIST selected AND
   xiaotai_mod_video_call IN_LIST selected AND xiaotai_mod_stream_video IN_LIST selected)
    xiaotai_host_test(app_companion_call SOURCES call_ui_test.c fakes.c ${core}
        ../src/companion_call_ui.c ../src/with_call.c ../src/with_wechat.c
        ../src/with_video_call.c ../src/with_stream_video.c
        INCLUDE . ../src INCLUDE_ONLY xiaotai_app_companion LINK ${test_deps} xiaotai_at)
    target_compile_definitions(app_companion_call PRIVATE COMPANION_TEST_REAL_AT=1)
    target_link_options(app_companion_call PRIVATE "LINKER:--wrap=xiaotai_time_us")
    if(XIAOTAI_HOST_SANITIZE)
        foreach(scenario call_keys call_headless call_present hold hold_preempt video at)
            add_test(NAME app_companion_${scenario} COMMAND app_companion_call ${scenario})
            set_tests_properties(app_companion_${scenario} PROPERTIES LABELS xiaotai_app_companion TIMEOUT 120)
        endforeach()
    endif()
endif()
