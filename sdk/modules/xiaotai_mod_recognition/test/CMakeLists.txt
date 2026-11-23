# SPDX-License-Identifier: MIT
xiaotai_host_test(mod_recognition_journal SOURCES journal_test.c
                  LINK xiaotai_mod_recognition INCLUDE ../src "${XIAOTAI_ROOT}/sdk/xiaotai_port/common")
target_compile_definitions(mod_recognition_journal PRIVATE XIAOTAI_RECOGNITION_JOURNAL_TEST)
target_link_options(mod_recognition_journal PRIVATE
    "LINKER:--wrap=xiaotai_kv_set_str" "LINKER:--wrap=xiaotai_kv_backend_get"
    "LINKER:--wrap=xiaotai_kv_backend_set" "LINKER:--wrap=xiaotai_kv_backend_commit")

xiaotai_host_test(mod_recognition_admission SOURCES admission_test.c
                  LINK xiaotai_mod_recognition INCLUDE ../src)
xiaotai_host_test(mod_recognition_policy SOURCES policy_test.c
                  LINK xiaotai_mod_recognition INCLUDE ../src)

add_executable(mod_recognition_operation operation_test.c fake_platform.c fake_media.c fake_binding.c)
target_link_libraries(mod_recognition_operation PRIVATE xiaotai_mod_recognition)
target_include_directories(mod_recognition_operation PRIVATE ../src)
target_link_options(mod_recognition_operation PRIVATE
    "LINKER:--wrap=xiaotai_time_us"
    "LINKER:--wrap=xiaotai_mem_alloc"
    "LINKER:--wrap=xiaotai_mem_calloc"
    "LINKER:--wrap=xiaotai_recognition_archive"
    "LINKER:--wrap=xiaotai_mem_free"
    "LINKER:--wrap=xiaotai_mem_stats"
    "LINKER:--wrap=xiaotai_kv_set_str"
    "LINKER:--wrap=xiaotai_kv_set_int"
    "LINKER:--wrap=xiaotai_kv_erase")
foreach(scenario admission owner-full corrupt stored foreign memory fragmented gap short
                 config-timeout capture-progress capture-cancel capture-timeout revoke
                 prepare-failed prepare-timeout prepare-offline recheck enqueue recheck-cancel
                 accepted not-sent clear-failure uncertain conflict config-unbound
                 transfer-unbound transfer-other-code upload-cancel upload-unsent-cancel
                 upload-timeout stale stale-action)
    add_test(NAME mod_recognition_operation_${scenario}
             COMMAND mod_recognition_operation ${scenario})
    set_tests_properties(mod_recognition_operation_${scenario} PROPERTIES TIMEOUT 30 LABELS xiaotai_mod_recognition)
endforeach()

foreach(case guards scope at legacy-notices)
    add_test(NAME mod_recognition_${case} COMMAND mod_recognition_operation ${case})
    set_tests_properties(mod_recognition_${case} PROPERTIES TIMEOUT 30 LABELS xiaotai_mod_recognition)
endforeach()
foreach(case scope late)
    add_test(NAME mod_recognition_guards_retired_${case} COMMAND mod_recognition_operation guards-retired-${case})
    set_tests_properties(mod_recognition_guards_retired_${case} PROPERTIES TIMEOUT 30 LABELS xiaotai_mod_recognition)
endforeach()

add_test(NAME mod_recognition_result COMMAND mod_recognition_operation result-views)
set_tests_properties(mod_recognition_result PROPERTIES TIMEOUT 30 LABELS xiaotai_mod_recognition)

add_test(NAME mod_recognition_query COMMAND mod_recognition_operation query-adopt)
set_tests_properties(mod_recognition_query PROPERTIES TIMEOUT 30 LABELS xiaotai_mod_recognition)
foreach(case RANGE 0 10)
    add_test(NAME mod_recognition_legacy_conflict_${case} COMMAND mod_recognition_operation legacy-conflict-${case})
    set_tests_properties(mod_recognition_legacy_conflict_${case} PROPERTIES TIMEOUT 30 LABELS xiaotai_mod_recognition)
endforeach()
foreach(scenario stream http conflict retry no-retry cancel timeout empty sync sync-admission sync-capacity
                 refresh upload-notice capture-notice foreign bad-service)
    add_test(NAME mod_recognition_query_${scenario} COMMAND mod_recognition_operation query-${scenario})
    set_tests_properties(mod_recognition_query_${scenario} PROPERTIES TIMEOUT 30 LABELS xiaotai_mod_recognition)
endforeach()

foreach(scenario unbound other clipped success timeout stale muted bad-headers repeated-encoding budget growth-no-mem)
    add_test(NAME mod_recognition_download_${scenario} COMMAND mod_recognition_operation audio-${scenario})
    set_tests_properties(mod_recognition_download_${scenario} PROPERTIES TIMEOUT 30 LABELS xiaotai_mod_recognition)
endforeach()

add_test(NAME mod_recognition_playback COMMAND mod_recognition_operation playback-manual)
add_test(NAME mod_recognition_decision COMMAND mod_recognition_operation decision-conflict)
set_tests_properties(mod_recognition_playback mod_recognition_decision PROPERTIES TIMEOUT 30 LABELS xiaotai_mod_recognition)
foreach(case current unadvanced unknown late replacement)
    add_test(NAME mod_recognition_decision_reused_scope_${case} COMMAND mod_recognition_operation decision-reused-${case})
    set_tests_properties(mod_recognition_decision_reused_scope_${case} PROPERTIES TIMEOUT 30 LABELS xiaotai_mod_recognition)
endforeach()
foreach(scenario playback-auto playback-order playback-chunked playback-revoke playback-mute playback-recording
                 playback-journal-fail playback-journal-timeout playback-dismiss-continue playback-dismiss-failed
                 decision-no-output decision-archive-retry)
    string(REPLACE "-" "_" test_name "${scenario}")
    add_test(NAME mod_recognition_${test_name} COMMAND mod_recognition_operation ${scenario})
    set_tests_properties(mod_recognition_${test_name} PROPERTIES TIMEOUT 30 LABELS xiaotai_mod_recognition)
endforeach()

xiaotai_host_test(mod_recognition_real_playback SOURCES playback_migration_test.c fake_platform.c fake_binding.c
                  LINK xiaotai_mod_recognition INCLUDE ../src)
target_link_options(mod_recognition_real_playback PRIVATE
    "LINKER:--wrap=xiaotai_mem_alloc" "LINKER:--wrap=xiaotai_mem_free" "LINKER:--wrap=xiaotai_mem_stats"
    "LINKER:--wrap=xiaotai_media_play_clips" "LINKER:--wrap=xiaotai_media_stop" "LINKER:--wrap=xiaotai_kv_set_str")
foreach(case preempt late-http malformed sink-error commit-error)
    string(REPLACE "-" "_" test_name "${case}")
    add_test(NAME mod_recognition_real_playback_${test_name} COMMAND mod_recognition_real_playback ${case})
    set_tests_properties(mod_recognition_real_playback_${test_name} PROPERTIES TIMEOUT 30 LABELS xiaotai_mod_recognition)
endforeach()

if(NOT XIAOTAI_HOST_SANITIZE)
    return()
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_executable(recognition_multipart_fixture multipart_test.c)
target_link_libraries(recognition_multipart_fixture PRIVATE xiaotai_mod_recognition)
target_include_directories(recognition_multipart_fixture PRIVATE ../src)
add_test(NAME mod_recognition_multipart COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/protocol_vectors.py" multipart "$<TARGET_FILE:recognition_multipart_fixture>")

add_executable(recognition_task_fixture task_test.c)
target_link_libraries(recognition_task_fixture PRIVATE xiaotai_mod_recognition)
target_include_directories(recognition_task_fixture PRIVATE ../src)
target_link_options(recognition_task_fixture PRIVATE "LINKER:--wrap=xiaotai_mem_stats")
add_test(NAME mod_recognition_task COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/protocol_vectors.py" task "$<TARGET_FILE:recognition_task_fixture>")
set_tests_properties(mod_recognition_multipart mod_recognition_task PROPERTIES
    TIMEOUT 120 LABELS xiaotai_mod_recognition)
