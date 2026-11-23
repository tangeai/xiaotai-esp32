# SPDX-License-Identifier: MIT
if(TARGET xiaotai_mod_room AND xiaotai_mod_room IN_LIST selected)
    xiaotai_host_test(app_companion_room SOURCES room_ui_test.c fakes.c ${core} ../src/with_room.c
        INCLUDE . ../src INCLUDE_ONLY xiaotai_app_companion LINK ${test_deps} xiaotai_eventlog)
    target_compile_definitions(app_companion_room PRIVATE COMPANION_TEST_PAGES=1)
    target_link_options(app_companion_room PRIVATE "LINKER:--wrap=xiaotai_time_us,--wrap=xiaotai_intent_post")
    if(XIAOTAI_HOST_SANITIZE)
        foreach(scenario room_options room_keys room_plain room_local_hold room_pointer_hold)
            add_test(NAME app_companion_${scenario} COMMAND app_companion_room ${scenario})
            set_tests_properties(app_companion_${scenario} PROPERTIES LABELS xiaotai_app_companion TIMEOUT 120)
        endforeach()
    endif()
endif()

if(TARGET xiaotai_mod_room AND xiaotai_mod_room IN_LIST selected AND
   TARGET xiaotai_mod_environment AND xiaotai_mod_environment IN_LIST selected)
    set(room_component "${XIAOTAI_ROOT}/sdk/modules/xiaotai_mod_room")
    set(room_platform "${room_component}/test/fake_platform.c")
    set(room_media "${room_component}/test/fake_media.c")
    set_source_files_properties("${room_platform}" PROPERTIES COMPILE_DEFINITIONS
        "fixture=room_http;xiaotai_platform_status=room_unused_platform_status")
    set_source_files_properties("${room_media}" PROPERTIES COMPILE_DEFINITIONS
        "xiaotai_media_status=room_unused_media_status")
    set(room_sources room.c room_assignment.c room_connect.c room_signal.c room_members.c room_talk.c room_at.c)
    list(TRANSFORM room_sources PREPEND "${room_component}/src/")
    xiaotai_host_test(app_companion_room_environment SOURCES room_environment_test.c fakes.c ${core}
        ../src/with_room.c ../src/with_environment.c ${room_sources}
        "${room_platform}" "${room_media}" "${room_component}/test/fake_transport.c"
        INCLUDE . ../src "${room_component}/src" "${room_component}/test"
        INCLUDE_ONLY xiaotai_app_companion xiaotai_mod_room xiaotai_mod_environment
        LINK ${test_deps} xiaotai_json xiaotai_at xiaotai_eventlog)
    target_compile_definitions(app_companion_room_environment PRIVATE COMPANION_TEST_PAGES=1 COMPANION_TEST_REAL_AT=1)
    target_link_options(app_companion_room_environment PRIVATE "LINKER:--wrap=xiaotai_intent_post,--wrap=xiaotai_time_us")
    if(TARGET xiaotai_theme_lcd_kit AND TARGET lvgl::lvgl)
        set(ui "${XIAOTAI_ROOT}/sdk/xiaotai_ui")
        xiaotai_host_test(app_companion_room_lcd SOURCES room_environment_test.c fakes.c ${core}
            "${XIAOTAI_ROOT}/tests/sdk/integration/room_pointer_ptt_test.c"
            ../src/with_room.c ../src/with_environment.c ../src/with_ui.c ${room_sources}
            "${room_platform}" "${room_media}" "${room_component}/test/fake_transport.c" "${ui}/test/ui_fake_board.cc"
            INCLUDE . ../src "${room_component}/src" "${room_component}/test" "${ui}/src" "${ui}/test"
            INCLUDE_ONLY xiaotai_app_companion xiaotai_mod_room xiaotai_mod_environment
            LINK ${test_deps} xiaotai_json xiaotai_at xiaotai_eventlog xiaotai_theme_lcd_kit lvgl::lvgl)
        target_compile_definitions(app_companion_room_lcd PRIVATE COMPANION_TEST_PAGES=1 COMPANION_TEST_REAL_AT=1
            COMPANION_TEST_REAL_UI=1 COMPANION_TEST_ROOM_LCD=1)
        target_link_options(app_companion_room_lcd PRIVATE "LINKER:--wrap=xiaotai_intent_post,--wrap=xiaotai_time_us")
    endif()
    foreach(scenario door form feedback-ended feedback-failed feedback-disabled feedback-preempt
            feedback-offline feedback-maintenance feedback-leave feedback-link-failed feedback-early-leave
            feedback-assignment feedback-admission-failed feedback-admission-deferred feedback-admission-disabled
            feedback-admission-memory-cue feedback-admission-platform-gated)
        add_test(NAME app_companion_room_environment_${scenario} COMMAND app_companion_room_environment ${scenario})
        set_tests_properties(app_companion_room_environment_${scenario}
            PROPERTIES LABELS xiaotai_app_companion TIMEOUT 120)
    endforeach()
endif()
