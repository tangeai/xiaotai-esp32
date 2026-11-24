# SPDX-License-Identifier: MIT
set(app "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(core ${core_sources})
list(TRANSFORM core PREPEND "${app}/")
list(APPEND core feedback_fakes.c audio_fakes.c)
set(test_deps xiaotai_port xiaotai_framework)
include(contacts_tests.cmake)
include(call_tests.cmake)
include(ai_contact_tools_tests.cmake)
include(ai_device_tools_tests.cmake)
include(environment_tests.cmake)
include(recognition_tests.cmake)
include(stream_tests.cmake)
include(room_tests.cmake)
set(fixture_dir "${CMAKE_CURRENT_SOURCE_DIR}/parts")
xiaotai_companion_parts(subset "${fixture_dir}" "${CMAKE_CURRENT_BINARY_DIR}/test_subset_parts.c"
    "${optional}" "xiaotai_mod_room;xiaotai_mod_contacts")
xiaotai_host_test(app_companion_pages SOURCES companion_test.c fakes.c ${core} ${subset}
    INCLUDE . ../src INCLUDE_ONLY xiaotai_app_companion LINK ${test_deps})
target_compile_definitions(app_companion_pages PRIVATE COMPANION_TEST_PAGES=1)
target_link_options(app_companion_pages PRIVATE "LINKER:--wrap=xiaotai_intent_post")

xiaotai_companion_parts(empty "${fixture_dir}" "${CMAKE_CURRENT_BINARY_DIR}/test_empty_parts.c" "${optional}" "")
xiaotai_host_test(app_companion_feedback SOURCES feedback_test.c fakes.c ${core} ${empty}
    INCLUDE . ../src INCLUDE_ONLY xiaotai_app_companion LINK ${test_deps})
xiaotai_host_test(app_companion_audio SOURCES audio_test.c fakes.c ${core}
    INCLUDE . ../src INCLUDE_ONLY xiaotai_app_companion LINK ${test_deps})
xiaotai_host_test(app_companion_style SOURCES companion_test.c fakes.c ${core} ${empty}
    INCLUDE ../src INCLUDE_ONLY xiaotai_app_companion LINK ${test_deps})
if(XIAOTAI_HOST_SANITIZE)
    add_test(NAME app_companion_plain COMMAND app_companion_style plain)
    set_tests_properties(app_companion_plain PROPERTIES LABELS xiaotai_app_companion TIMEOUT 120)
endif()

xiaotai_companion_parts(setup_parts "${fixture_dir}" "${CMAKE_CURRENT_BINARY_DIR}/test_setup_parts.c"
    "${optional}" "xiaotai_ui;xiaotai_mod_room")
xiaotai_host_test(app_companion_setup SOURCES setup_test.c fakes.c ${core} ${setup_parts}
    INCLUDE . ../src INCLUDE_ONLY xiaotai_app_companion LINK ${test_deps})
target_compile_definitions(app_companion_setup PRIVATE SETUP_TEST_OPTIONAL=1)
xiaotai_host_test(app_companion_setup_without SOURCES setup_test.c fakes.c ${core} ${empty}
    INCLUDE . ../src INCLUDE_ONLY xiaotai_app_companion LINK ${test_deps})
target_compile_definitions(app_companion_setup_without PRIVATE SETUP_TEST_OPTIONAL=0)
foreach(target app_companion_setup app_companion_setup_without)
    foreach(scenario keys headless binding_code binding_header_retry binding_refresh_retry)
        add_test(NAME ${target}_${scenario} COMMAND ${target} ${scenario})
        set_tests_properties(${target}_${scenario} PROPERTIES LABELS xiaotai_app_companion TIMEOUT 120)
    endforeach()
endforeach()

set(preference_support "${XIAOTAI_ROOT}/tests/sdk/support")
xiaotai_host_test(app_companion_home SOURCES home_settings_test.c fakes.c ${core} ../src/with_ui.c
    "${preference_support}/companion_preferences_fixture.c"
    INCLUDE . ../src "${preference_support}" INCLUDE_ONLY xiaotai_app_companion LINK ${test_deps} xiaotai_identity)
target_include_directories(app_companion_home PRIVATE "${XIAOTAI_ROOT}/sdk/xiaotai_port/common")
target_link_options(app_companion_home PRIVATE "LINKER:--wrap=xiaotai_time_us"
    "LINKER:--wrap=xiaotai_kv_backend_commit")
if(TARGET xiaotai_mod_ai_talk AND TARGET xiaotai_mod_wake AND
   xiaotai_mod_ai_talk IN_LIST selected AND xiaotai_mod_wake IN_LIST selected)
    xiaotai_host_test(app_companion_talk SOURCES talk_test.c fakes.c ${core}
        ../src/with_ai_talk.c ../src/with_wake.c
        INCLUDE . ../src INCLUDE_ONLY xiaotai_app_companion LINK ${test_deps})
    target_compile_definitions(app_companion_talk PRIVATE COMPANION_WITH_AI_CONTACT_TOOLS=1)
    target_link_options(app_companion_talk PRIVATE "LINKER:--wrap=xiaotai_time_us")
    if(TARGET xiaotai_interaction AND XIAOTAI_HOST_SANITIZE)
        xiaotai_host_test(app_companion_main_press SOURCES talk_test.c fakes.c ${core}
            ../src/with_ai_talk.c ../src/with_wake.c
            INCLUDE . ../src "${XIAOTAI_ROOT}/sdk/xiaotai_interaction/src"
            INCLUDE_ONLY xiaotai_app_companion LINK ${test_deps} xiaotai_interaction)
        target_compile_definitions(app_companion_main_press PRIVATE
            COMPANION_TEST_REAL_INTERACTION=1 COMPANION_TEST_REAL_AT=1)
        target_link_options(app_companion_main_press PRIVATE "LINKER:--wrap=xiaotai_time_us"
            "LINKER:--wrap=interaction_poll_start")
        add_test(NAME app_companion_main_click_default COMMAND app_companion_main_press click)
        set_tests_properties(app_companion_main_click_default PROPERTIES LABELS xiaotai_app_companion TIMEOUT 120)
    endif()
    if(XIAOTAI_HOST_SANITIZE)
        foreach(scenario session caption_prefix caption_prefix_keys talk_keys talk_keys_hold wake_admission)
            add_test(NAME app_companion_${scenario} COMMAND app_companion_talk ${scenario})
            set_tests_properties(app_companion_${scenario} PROPERTIES LABELS xiaotai_app_companion TIMEOUT 120)
        endforeach()
    endif()
endif()
if(TARGET xiaotai_ui)
    set(ui "${app}/../../sdk/xiaotai_ui")
    foreach(test app_companion_wechat_registry app_companion_wechat_absent app_companion_wechat_original)
        xiaotai_host_test(${test} SOURCES wechat_code_registry_test.c fakes.c ${core} ../src/with_ui.c
            "${ui}/test/ui_fake_board.cc" "${ui}/test/ui_fake_theme.c"
            INCLUDE . ../src "${ui}/src" "${ui}/test" INCLUDE_ONLY xiaotai_app_companion
            LINK ${test_deps} xiaotai_ui)
        target_compile_definitions(${test} PRIVATE COMPANION_TEST_REAL_UI=1)
    endforeach()
    target_sources(app_companion_wechat_registry PRIVATE wechat_code_fixture.c)
    target_compile_definitions(app_companion_wechat_registry PRIVATE XIAOTAI_COMPANION_WECHAT_CODE_ASSET=1)
    target_sources(app_companion_wechat_original PRIVATE "${app}/assets/wechat_code.c")
    target_compile_definitions(app_companion_wechat_original PRIVATE XIAOTAI_COMPANION_WECHAT_CODE_ASSET=1
        COMPANION_TEST_ORIGINAL_CODE=1)
endif()
if(XIAOTAI_HOST_SANITIZE)
    foreach(scenario settings choice_reconfirm save_failure commit_failure commit_failure_superseded
                     stale_same_key current_failure other_key newer_failed no_op rejected retired
                     metadata_oom notify_home notify_keys)
        add_test(NAME app_companion_${scenario} COMMAND app_companion_home ${scenario})
        set_tests_properties(app_companion_${scenario} PROPERTIES LABELS xiaotai_app_companion TIMEOUT 120)
    endforeach()
    add_test(NAME app_companion_wechat_code COMMAND "${XIAOTAI_HOST_PYTHON}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/test_wechat_code.py")
    set_tests_properties(app_companion_wechat_code PROPERTIES LABELS xiaotai_app_companion TIMEOUT 30)
    # The legacy S3, P4 and WT99 UIs shipped byte-identical copies of this image.
    add_test(NAME app_companion_wechat_code_legacy COMMAND "${XIAOTAI_HOST_PYTHON}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/wechat_code_image.py" "${CMAKE_CURRENT_SOURCE_DIR}/legacy_wechat_qr.inc")
    set_tests_properties(app_companion_wechat_code_legacy PROPERTIES LABELS xiaotai_app_companion TIMEOUT 30)
endif()
