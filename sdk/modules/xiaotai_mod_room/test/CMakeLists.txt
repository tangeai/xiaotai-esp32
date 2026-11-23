# SPDX-License-Identifier: MIT
xiaotai_host_test(mod_room_assignment
    SOURCES room_test.c fake_platform.c fake_transport.c fake_media.c ../src/room.c ../src/room_assignment.c ../src/room_connect.c
            ../src/room_signal.c ../src/room_members.c ../src/room_talk.c ../src/room_at.c
    INCLUDE ../src INCLUDE_ONLY xiaotai_mod_room
    LINK xiaotai_port xiaotai_framework xiaotai_json xiaotai_at xiaotai_eventlog)
target_compile_definitions(mod_room_assignment PRIVATE XIAOTAI_TEST_ROOM_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/../module.json")
target_link_options(mod_room_assignment PRIVATE "LINKER:--wrap=xiaotai_time_us" "LINKER:--wrap=xiaotai_mem_has_external"
    "LINKER:--wrap=xiaotai_clock_unix_ms" "LINKER:--wrap=xiaotai_task_create" "LINKER:--wrap=xiaotai_mem_alloc"
    "LINKER:--wrap=xiaotai_mem_free")
foreach(case attend_stale config connect codec_alias retry release preempt quiesce members legacy_owner legacy_owner_reconnect legacy_xmini_heartbeat legacy_xmini_ptt command_recovery talk local_hold local_refresh count_only at intents)
    add_test(NAME mod_room_${case} COMMAND mod_room_assignment ${case})
    set_tests_properties(mod_room_${case} PROPERTIES LABELS xiaotai_mod_room TIMEOUT 120)
endforeach()
add_test(NAME mod_room_local_refresh_exclusive COMMAND mod_room_assignment local_refresh_exclusive)
set_tests_properties(mod_room_local_refresh_exclusive PROPERTIES LABELS xiaotai_mod_room TIMEOUT 120)
add_test(NAME mod_room_token_http_lease COMMAND mod_room_assignment token_http_lease)
set_tests_properties(mod_room_token_http_lease PROPERTIES LABELS xiaotai_mod_room TIMEOUT 120)
add_test(NAME mod_room_legacy_superseded_lease COMMAND mod_room_assignment legacy_superseded_lease)
add_test(NAME mod_room_legacy_superseded_unknown COMMAND mod_room_assignment legacy_superseded_unknown)
add_test(NAME mod_room_heartbeat_expiry_retry COMMAND mod_room_assignment heartbeat_expiry_retry)
add_test(NAME mod_room_connecting_presence COMMAND mod_room_assignment connecting_presence)
set_tests_properties(mod_room_legacy_superseded_lease mod_room_legacy_superseded_unknown
    mod_room_heartbeat_expiry_retry mod_room_connecting_presence
    PROPERTIES LABELS xiaotai_mod_room TIMEOUT 120)
