# SPDX-License-Identifier: MIT
# Host tests of the identity component (work package I-1), on the host storage
# port (typed in-memory entries with failure injection) and the host memory
# port's allocation counts, under the address and undefined-behaviour
# sanitizers. The internal header (../src) gives the tests the layout
# classifiers and the state transitions the storage jobs apply.
xiaotai_host_test(identity_layout SOURCES identity_layout_test.c LINK xiaotai_identity INCLUDE ../src)
xiaotai_host_test(identity_fingerprint SOURCES identity_fingerprint_test.c LINK xiaotai_identity INCLUDE ../src)
xiaotai_host_test(identity_generation SOURCES identity_generation_test.c LINK xiaotai_identity INCLUDE ../src)
target_link_options(identity_generation PRIVATE "LINKER:--wrap=xiaotai_mutex_unlock")
xiaotai_host_test(identity_core SOURCES identity_core_test.c LINK xiaotai_identity INCLUDE ../src)
xiaotai_host_test(identity_save SOURCES identity_jobs_test.c LINK xiaotai_identity INCLUDE ../src)
target_link_options(identity_save PRIVATE "LINKER:--wrap=xiaotai_task_create" "LINKER:--wrap=xiaotai_task_exit")
foreach(case erase reload erase_for_restart)
    add_test(NAME identity_${case} COMMAND identity_save ${case})
    set_tests_properties(identity_${case} PROPERTIES LABELS xiaotai_identity TIMEOUT 10)
endforeach()
xiaotai_host_test(identity_save_failures SOURCES identity_persistence_faults_test.c LINK xiaotai_identity
                  INCLUDE ../src ../../xiaotai_port/common TIMEOUT 10)
target_link_options(identity_save_failures PRIVATE "LINKER:--wrap=xiaotai_kv_backend_commit")
add_test(NAME identity_erase_failures COMMAND identity_save_failures erase)
set_tests_properties(identity_erase_failures PROPERTIES LABELS xiaotai_identity TIMEOUT 10)

# I-3: layout cases and owner/worker lifecycle against the real framework and
# the typed host KV backend. Each case runs with a fresh process and storage.
xiaotai_host_test(preferences_default SOURCES preferences_layout_test.c LINK xiaotai_identity INCLUDE ../src)
foreach(case int string record record_malformed record_recovery record_maintenance_retry)
    add_test(NAME preferences_${case} COMMAND preferences_default ${case})
    set_tests_properties(preferences_${case} PROPERTIES LABELS xiaotai_identity TIMEOUT 10)
endforeach()
xiaotai_host_test(preferences_pending SOURCES preferences_lifecycle_test.c LINK xiaotai_identity INCLUDE ../src)
target_link_options(preferences_pending PRIVATE "LINKER:--wrap=xiaotai_task_create")
foreach(case coalesce no_task owner_only flush listener setting_store maintenance_failure)
    add_test(NAME preferences_${case} COMMAND preferences_pending ${case})
    set_tests_properties(preferences_${case} PROPERTIES LABELS xiaotai_identity TIMEOUT 10)
endforeach()
xiaotai_host_test(preferences_snapshot_deadline SOURCES preferences_snapshot_deadline_test.c
                  LINK xiaotai_identity INCLUDE ../src TIMEOUT 10)
target_link_options(preferences_snapshot_deadline PRIVATE "LINKER:--wrap=xiaotai_time_us"
    "LINKER:--wrap=xiaotai_kv_commit_async")
foreach(case open admitted staging)
    add_test(NAME preferences_snapshot_deadline_${case} COMMAND preferences_snapshot_deadline ${case})
    set_tests_properties(preferences_snapshot_deadline_${case} PROPERTIES LABELS xiaotai_identity TIMEOUT 10)
endforeach()
