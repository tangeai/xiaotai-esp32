# SPDX-License-Identifier: MIT
if(xiaotai_mod_stream IN_LIST selected)
    xiaotai_host_test(app_companion_stream SOURCES stream_test.c fakes.c ${core}
        ../src/with_stream.c INCLUDE . ../src INCLUDE_ONLY xiaotai_app_companion LINK ${test_deps})
    target_link_options(app_companion_stream PRIVATE "LINKER:--wrap=xiaotai_time_us")
    if(TARGET xiaotai_ui AND XIAOTAI_HOST_SANITIZE)
        set(ui "${app}/../../sdk/xiaotai_ui")
        xiaotai_host_test(app_companion_stream_touch SOURCES stream_touch_test.c fakes.c ${core}
            ../src/with_stream.c ../src/with_ui.c "${ui}/test/ui_fake_board.cc" "${ui}/test/ui_fake_theme.c"
            INCLUDE . ../src "${ui}/src" "${ui}/test" INCLUDE_ONLY xiaotai_app_companion
            LINK ${test_deps} xiaotai_ui)
        target_compile_definitions(app_companion_stream_touch PRIVATE COMPANION_TEST_REAL_UI=1)
        foreach(scenario full stale rollback expiry retry local-show local-stale local-page)
            add_test(NAME app_companion_stream_touch_${scenario} COMMAND app_companion_stream_touch ${scenario})
            set_tests_properties(app_companion_stream_touch_${scenario} PROPERTIES LABELS xiaotai_app_companion TIMEOUT 20)
        endforeach()
    endif()
    if(XIAOTAI_HOST_SANITIZE)
        foreach(scenario keys present)
            add_test(NAME app_companion_stream_${scenario} COMMAND app_companion_stream ${scenario})
            set_tests_properties(app_companion_stream_${scenario} PROPERTIES LABELS xiaotai_app_companion TIMEOUT 120)
        endforeach()
    endif()
    if(xiaotai_mod_stream_video IN_LIST selected)
        xiaotai_host_test(app_companion_stream_video SOURCES stream_test.c fakes.c ${core}
            ../src/with_stream.c ../src/with_stream_video.c
            INCLUDE . ../src INCLUDE_ONLY xiaotai_app_companion LINK ${test_deps})
        target_compile_definitions(app_companion_stream_video PRIVATE COMPANION_TEST_STREAM_VIDEO=1)
        target_link_options(app_companion_stream_video PRIVATE "LINKER:--wrap=xiaotai_time_us")
        if(XIAOTAI_HOST_SANITIZE)
            add_test(NAME app_companion_stream_fixed COMMAND app_companion_stream_video fixed)
            set_tests_properties(app_companion_stream_fixed PROPERTIES LABELS xiaotai_app_companion TIMEOUT 120)
        endif()
    endif()
endif()
