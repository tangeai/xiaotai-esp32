# SPDX-License-Identifier: MIT
xiaotai_host_test(mod_stream_admission
    SOURCES stream_test.c ../src/stream.c ../src/stream_profile.c ../src/stream_at.c
    INCLUDE_ONLY xiaotai_mod_stream
    LINK xiaotai_port xiaotai_framework xiaotai_eventlog xiaotai_json)
target_link_options(mod_stream_admission PRIVATE "LINKER:--wrap=xiaotai_mem_has_external,--wrap=xiaotai_activity_begin")
foreach(case session profile receive_policy hold preflight)
    add_test(NAME mod_stream_${case} COMMAND mod_stream_admission ${case})
    set_tests_properties(mod_stream_${case} PROPERTIES TIMEOUT 120 LABELS xiaotai_mod_stream)
endforeach()

# Keep composition fixtures in tests/sdk; they may assemble engines that the
# stream module does not own or import in production.
xiaotai_host_test(mod_stream_preflight_integration
    SOURCES ../../../../tests/sdk/stream_preflight_integration_test.c
    LINK xiaotai_mod_stream xiaotai_mod_environment
    INCLUDE ../../../xiaotai_platform/src ../../../xiaotai_transport/src TIMEOUT 20)
target_link_options(mod_stream_preflight_integration PRIVATE
    "LINKER:--wrap=xiaotai_task_create,--wrap=xiaotai_platform_signaling_hold,--wrap=xiaotai_platform_signaling_release"
    "LINKER:--wrap=xiaotai_platform_profile_scene,--wrap=xiaotai_platform_scene_parts")
target_link_options(mod_stream_preflight_integration PRIVATE
    "LINKER:--wrap=xiaotai_preferences_maintenance_flush,--wrap=xiaotai_platform_environment_store,--wrap=xt_now_ms"
    "LINKER:--wrap=xiaotai_platform_endpoint,--wrap=xiaotai_critical_exit")
if(TEST mod_stream_preflight_integration)
    foreach(order inbound-first close-first rollback)
        add_test(NAME mod_stream_environment_${order} COMMAND mod_stream_preflight_integration environment-${order})
        set_tests_properties(mod_stream_environment_${order} PROPERTIES TIMEOUT 20 LABELS xiaotai_mod_stream)
    endforeach()
    foreach(policy offline-native offline-s3 profile-native profile-s3)
        add_test(NAME mod_stream_environment_release_${policy} COMMAND mod_stream_preflight_integration environment-release-${policy})
        set_tests_properties(mod_stream_environment_release_${policy} PROPERTIES TIMEOUT 20 LABELS xiaotai_mod_stream)
    endforeach()
    foreach(case held-zero held-control held-explicit-error held-default held-stop-ack held-next held-pending
                 held-pending-ack held-stop-before-request held-stale-stop held-admission-before-error
                 held-inline-ack held-stuck-ack)
        add_test(NAME mod_stream_environment_finish_${case} COMMAND mod_stream_preflight_integration environment-finish-${case})
        set_tests_properties(mod_stream_environment_finish_${case} PROPERTIES TIMEOUT 20 LABELS xiaotai_mod_stream)
    endforeach()
    foreach(order stop-first transport-first busy busy-before-ack peer-error error-then-ack listener-off)
        add_test(NAME mod_stream_replacement_${order} COMMAND mod_stream_preflight_integration ${order})
        set_tests_properties(mod_stream_replacement_${order} PROPERTIES TIMEOUT 20 LABELS xiaotai_mod_stream)
    endforeach()
    foreach(case gate default offline)
        add_test(NAME mod_stream_local_stop_${case} COMMAND mod_stream_preflight_integration local-stop-${case})
        set_tests_properties(mod_stream_local_stop_${case} PROPERTIES TIMEOUT 20 LABELS xiaotai_mod_stream)
    endforeach()
endif()
