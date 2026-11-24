# SPDX-License-Identifier: MIT
set(call_sources ../src/call.c ../src/call_signal.c ../src/call_http.c ../src/call_release.c
    ../src/call_profile.c ../src/call_at.c ../src/call_signaling.c)
xiaotai_host_test(mod_call_incoming SOURCES call_test.c ${call_sources}
    INCLUDE ../src INCLUDE_ONLY xiaotai_mod_call
    LINK xiaotai_port xiaotai_framework xiaotai_json xiaotai_eventlog xiaotai_at)
target_link_options(mod_call_incoming PRIVATE "LINKER:--wrap=xiaotai_time_us")
target_compile_definitions(mod_call_incoming PRIVATE XIAOTAI_CALL_HAS_SETTINGS=1
    XIAOTAI_TEST_CALL_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/../module.json")
xiaotai_host_test(mod_call_no_settings SOURCES call_test.c ${call_sources}
    INCLUDE ../src INCLUDE_ONLY xiaotai_mod_call
    LINK xiaotai_port xiaotai_framework xiaotai_json xiaotai_eventlog xiaotai_at)
target_link_options(mod_call_no_settings PRIVATE "LINKER:--wrap=xiaotai_time_us")
target_compile_definitions(mod_call_no_settings PRIVATE CALL_TEST_HEADLESS=1
    XIAOTAI_TEST_CALL_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/../module.json")
foreach(case accept accept_bad_room_release outgoing_connect end platform mute mute_policy contacts busy_during media eventlog dial queued_dial stale_room
             release release_deferred release_timeouts release_hold_capacity profile profile_voice_geometry listen at message)
    add_test(NAME mod_call_${case} COMMAND mod_call_incoming ${case})
    set_tests_properties(mod_call_${case} PROPERTIES LABELS xiaotai_mod_call TIMEOUT 120)
endforeach()
add_test(NAME mod_call_at_hangup_default COMMAND mod_call_incoming at_hangup)
add_test(NAME mod_call_at_hangup_reject_incoming COMMAND mod_call_incoming at_hangup strict)
set_tests_properties(mod_call_at_hangup_default mod_call_at_hangup_reject_incoming
    PROPERTIES LABELS xiaotai_mod_call TIMEOUT 120)
foreach(case accept_connection_failure legacy_shutdown signaling_hold)
    add_test(NAME mod_call_${case} COMMAND mod_call_incoming ${case})
    set_tests_properties(mod_call_${case} PROPERTIES LABELS xiaotai_mod_call TIMEOUT 120)
endforeach()
