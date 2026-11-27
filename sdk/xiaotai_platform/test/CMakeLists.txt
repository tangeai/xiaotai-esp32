# SPDX-License-Identifier: MIT
# Host tests of the platform core. Work package P-1: address admission, the
# strict business code, signaling envelopes and the stored environment. Tests
# reach the component's internal interfaces through ../src.
xiaotai_host_test(platform_url SOURCES platform_url_test.c LINK xiaotai_platform INCLUDE ../src)
xiaotai_host_test(platform_json_code SOURCES platform_json_code_test.c LINK xiaotai_platform)
xiaotai_host_test(platform_envelope SOURCES platform_envelope_test.c LINK xiaotai_platform INCLUDE ../src)
xiaotai_host_test(platform_signature_contract SOURCES platform_signature_test.c LINK xiaotai_platform INCLUDE ../src)
target_link_options(platform_signature_contract PRIVATE "LINKER:--wrap=xiaotai_identity_get"
                    "LINKER:--wrap=xiaotai_clock_unix_ms" "LINKER:--wrap=xiaotai_random")
xiaotai_host_test(platform_http_trace SOURCES platform_http_trace_test.c LINK xiaotai_platform INCLUDE ../src)
xiaotai_host_test(platform_diagnostic_format SOURCES platform_diagnostics_test.c LINK xiaotai_platform INCLUDE ../src)
target_link_options(platform_diagnostic_format PRIVATE "LINKER:--wrap=xiaotai_net_provisioning_boot")
foreach(scenario default provisioning max)
    add_test(NAME platform_diagnostic_${scenario} COMMAND platform_diagnostic_format ${scenario})
    set_tests_properties(platform_diagnostic_${scenario} PROPERTIES LABELS xiaotai_platform TIMEOUT 20)
endforeach()
xiaotai_host_test(platform_signal_scan SOURCES platform_signal_scan_test.c LINK xiaotai_platform INCLUDE ../src)
xiaotai_host_test(platform_signal_pool SOURCES platform_signal_pool_test.c LINK xiaotai_platform INCLUDE ../src)
target_link_options(platform_signal_pool PRIVATE "LINKER:--wrap=xiaotai_identity_generation" "LINKER:--wrap=xiaotai_time_us"
                    "LINKER:--wrap=xiaotai_run" "LINKER:--wrap=xiaotai_mqtt_publish")
xiaotai_host_test(platform_environment SOURCES platform_environment_test.c LINK xiaotai_platform
                  INCLUDE ../src ../../xiaotai_port/common)
target_link_options(platform_environment PRIVATE "LINKER:--wrap=xiaotai_kv_backend_get"
                    "LINKER:--wrap=xiaotai_kv_backend_get_str_strict" "LINKER:--wrap=xiaotai_kv_backend_commit")
xiaotai_host_test(platform_request_admission SOURCES platform_request_test.c LINK xiaotai_platform INCLUDE ../src)
target_link_options(platform_request_admission PRIVATE "LINKER:--wrap=xiaotai_identity_generation"
                   "LINKER:--wrap=xiaotai_identity_state" "LINKER:--wrap=xiaotai_identity_get"
                   "LINKER:--wrap=xiaotai_mem_has_external" "LINKER:--wrap=xiaotai_task_create"
                   "LINKER:--wrap=platform_signaling_step" "LINKER:--wrap=xiaotai_http_perform"
                   "LINKER:--wrap=xiaotai_time_us" "LINKER:--wrap=xiaotai_task_current_stack_free"
                   "LINKER:--wrap=xiaotai_task_list"
                   "LINKER:--wrap=xiaotai_mem_alloc,--wrap=xiaotai_mem_calloc,--wrap=xiaotai_mem_free")
foreach(scenario order reply stale admission_epoch connection no_retry queued_reuse queued_release transfer preflight resume_first timeout owner_queue
                 background reply_pressure ai_reply_pressure ai_reply_pressure_200 idle_pressure provisioning
                 worker_unavailable stack_watermark body_demand body_resident
                 body_demand_legacy saturation_legacy saturation_default legacy_http_no_delay)
    add_test(NAME platform_request_${scenario} COMMAND platform_request_admission ${scenario})
    set_tests_properties(platform_request_${scenario} PROPERTIES LABELS xiaotai_platform TIMEOUT 20)
endforeach()
foreach(scenario upload download cancel start_race lifecycle heartbeat)
    add_test(NAME platform_transfer_${scenario} COMMAND platform_request_admission transfer_${scenario})
    set_tests_properties(platform_transfer_${scenario} PROPERTIES LABELS xiaotai_platform TIMEOUT 20)
endforeach()

xiaotai_host_test(platform_session_boot SOURCES platform_session_test.c LINK xiaotai_platform INCLUDE ../src)
target_link_options(platform_session_boot PRIVATE "LINKER:--wrap=xiaotai_time_us" "LINKER:--wrap=xiaotai_random"
                   "LINKER:--wrap=xiaotai_signal_take" "LINKER:--wrap=platform_signaling_default_link"
                   "LINKER:--wrap=xiaotai_mqtt_create" "LINKER:--wrap=xiaotai_mqtt_subscribe" "LINKER:--wrap=xiaotai_mqtt_destroy"
                   "LINKER:--wrap=xiaotai_mqtt_start" "LINKER:--wrap=xiaotai_mqtt_stop"
                   "LINKER:--wrap=xiaotai_mqtt_publish"
                   "LINKER:--wrap=xiaotai_mem_has_external" "LINKER:--wrap=xiaotai_http_create"
                   "LINKER:--wrap=xiaotai_http_perform"
                   "LINKER:--wrap=xiaotai_delay_ms" "LINKER:--wrap=xiaotai_mem_stats"
                   "LINKER:--wrap=platform_signaling_heap_reaped")
foreach(scenario provision boot_error boot_error_type clock_config auth_ignore auth_pause auth_pause_link auth_relogin
                 refresh_on refresh_off refresh_c3_pause refresh_cooldown expiry
                 broker_gate broker_start_gate
                 missing unbound mqtt_bad mqtt_unchecked mqtt_empty mqtt_over missing_service discovery_stale login_duplicate
                 login_requires_expiry transport_not_found fingerprint_upper fingerprint_lower fingerprint_compact
                 no_provider signature start_fails wire_batch wire_batch_hosted_retry wire_batch_hosted_stop
                 wire_batch_hosted_binding wire_split wire_split_reconnect wire_split_request_failure
                 wire_split_legacy_recovery wire_split_legacy_exhaustion wire_split_legacy_refusal
                 wire_split_legacy_stop wire_split_legacy_unbound_ack
                 wire_split_refused wire_split_excess_qos wire_split_excess_count
                 wire_split_malformed_ack wire_split_stale_ack_error wire_split_uncorrelated_error
                 wire_retry wire_refused wire_hold_reuse
                 wire_hold_relogin wire_hold_early wire_hold_startup wire_hold_stop_failure wire_hold_start_failure
                 wire_hold_mqtt_lifetime
                 wire_hold_bounded_ttl wire_hold_bounded_refresh wire_hold_bounded_expiry wire_hold_bounded_opaque
                 wire_hold_bounded_network wire_hold_bounded_exchange wire_hold_bounded_stop_failure wire_hold_bounded_scoped
                 wire_hold_bounded_stop_persistent
                 wire_heartbeat_ready wire_heartbeat_skipped wire_lifecycle wire_signal)
    add_test(NAME platform_session_${scenario} COMMAND platform_session_boot ${scenario})
    set_tests_properties(platform_session_${scenario} PROPERTIES LABELS xiaotai_platform TIMEOUT 20)
endforeach()
add_test(NAME platform_session_refresh_c3_media COMMAND platform_session_boot refresh_c3_media)
set_tests_properties(platform_session_refresh_c3_media PROPERTIES LABELS xiaotai_platform TIMEOUT 20)
foreach(scenario ack early_ack timeout old_client notify http http_stop_retry http_stop_gate http_stop_gate_inflight
                 http_stop_gate_persistent verify_online
                 verify_unbound upgrade queued_upgrade owner_pressure start)
    add_test(NAME platform_session_wire_binding_${scenario} COMMAND platform_session_boot wire_binding_${scenario})
    set_tests_properties(platform_session_wire_binding_${scenario} PROPERTIES LABELS xiaotai_platform TIMEOUT 20)
endforeach()
foreach(scenario valid expired same_second missing text_expiry fractional overflow trailing embedded_nul bad_jwt
                 success_http201 success_code_fractional)
    add_test(NAME platform_session_c3_token_${scenario} COMMAND platform_session_boot c3_token_${scenario})
    set_tests_properties(platform_session_c3_token_${scenario} PROPERTIES LABELS xiaotai_platform TIMEOUT 20)
endforeach()
foreach(scenario server_error wrong_status fractional string duplicate trailing transport_timeout success_fractional)
    add_test(NAME platform_session_login_wire_${scenario} COMMAND platform_session_boot login_wire_${scenario})
    set_tests_properties(platform_session_login_wire_${scenario} PROPERTIES LABELS xiaotai_platform TIMEOUT 20)
endforeach()
foreach(scenario document mz28 retry biz_retry no_biz_retry no_retry unbound defer writer_error stale_writer queue
                 large large_cap wait_rtc rtc_first fast_hold preflight_hold prewire_hold preflight_gated
                 response_matrix reply_pressure reply_pressure_stale reply_pressure_retry)
    add_test(NAME platform_session_wire_profile_${scenario} COMMAND platform_session_boot wire_profile_${scenario})
    set_tests_properties(platform_session_wire_profile_${scenario} PROPERTIES LABELS xiaotai_platform TIMEOUT 20)
endforeach()
if(CONFIG_XIAOTAI_PLATFORM_RECONNECT_COMMAND)
    foreach(scenario help default stress invalid busy offline stop_failure timeout unavailable)
        add_test(NAME platform_session_wire_at_${scenario} COMMAND platform_session_boot wire_at_${scenario})
        set_tests_properties(platform_session_wire_at_${scenario} PROPERTIES LABELS xiaotai_platform TIMEOUT 20)
    endforeach()
else()
    add_test(NAME platform_session_wire_at_help_disabled COMMAND platform_session_boot wire_at_help)
    set_tests_properties(platform_session_wire_at_help_disabled PROPERTIES LABELS xiaotai_platform TIMEOUT 20)
endif()
xiaotai_host_test(platform_guest SOURCES platform_guest_test.c LINK xiaotai_platform INCLUDE ../src)
target_link_options(platform_guest PRIVATE "LINKER:--wrap=xiaotai_mqtt_create" "LINKER:--wrap=xiaotai_mqtt_subscribe"
    "LINKER:--wrap=xiaotai_mqtt_stop" "LINKER:--wrap=xiaotai_mqtt_destroy"
    "LINKER:--wrap=xiaotai_identity_state" "LINKER:--wrap=xiaotai_identity_get" "LINKER:--wrap=xiaotai_time_us")
foreach(scenario refused sub_refused timeout offline login manual owner_pressure ack_timeout ack_enqueue_rejected
                 subscribe_13s subscribe_before_20s subscribe_at_20s subscribe_jwt_cap subscribe_owner_deadline)
    add_test(NAME platform_guest_${scenario} COMMAND platform_guest ${scenario})
    set_tests_properties(platform_guest_${scenario} PROPERTIES LABELS xiaotai_platform TIMEOUT 20)
endforeach()
