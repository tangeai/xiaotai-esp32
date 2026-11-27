# SPDX-License-Identifier: MIT
# Host tests of the transport engine.
#   transport_codec              the codec vocabulary the links share with the
#                                media engine and the parts (XR-CODEC)
#   transport_run                configuration, sizing and run rules (WP1)
#   transport_provisioning_boot  nothing taken in a provisioning boot (WP1)
# The run-control tests link the real core with the scripted backend
# (host/script_backend.c), the real framework and event log, and fakes of the
# identity, platform and network engines (fakes.c); ../src gives them the
# core's internal interface.
xiaotai_host_test(transport_codec SOURCES codec_test.c LINK xiaotai_transport)
xiaotai_host_test(transport_run SOURCES run_test.c fakes.c LINK xiaotai_transport INCLUDE ../src TIMEOUT 300)
target_link_options(transport_run PRIVATE "LINKER:--wrap=xiaotai_mem_stats,--wrap=xiaotai_platform_signaling_hold")
target_link_options(transport_run PRIVATE "LINKER:--wrap=xiaotai_task_create")
add_test(NAME transport_connect_memory_thresholds COMMAND transport_run connect-memory-thresholds)
set_tests_properties(transport_connect_memory_thresholds PROPERTIES LABELS xiaotai_transport TIMEOUT 30)
add_test(NAME transport_connect_memory_owner COMMAND transport_run connect-memory-owner)
set_tests_properties(transport_connect_memory_owner PROPERTIES LABELS xiaotai_transport TIMEOUT 30)
add_test(NAME transport_start_worker_failure COMMAND transport_run start-worker-failure)
set_tests_properties(transport_start_worker_failure PROPERTIES LABELS xiaotai_transport TIMEOUT 30)
xiaotai_host_test(transport_provisioning_boot SOURCES provisioning_boot_test.c fakes.c LINK xiaotai_transport
                  INCLUDE ../src)
xiaotai_host_test(transport_inbound SOURCES inbound_test.c fakes.c LINK xiaotai_transport INCLUDE ../src)
target_link_options(transport_inbound PRIVATE
    "LINKER:--wrap=xiaotai_event_emit,--wrap=xiaotai_critical_enter,--wrap=xiaotai_critical_exit")
add_test(NAME transport_cancel_block COMMAND transport_inbound cancel_block)
add_test(NAME transport_admission_orders COMMAND transport_inbound orders)
add_test(NAME transport_retired_open COMMAND transport_inbound retired_open)
add_test(NAME transport_retired_open_default COMMAND transport_inbound retired_open_default)
add_test(NAME transport_p4_call_selector COMMAND transport_inbound p4_call_selector)
add_test(NAME transport_p4_pending_h5_cleanup COMMAND transport_inbound p4_pending_h5_cleanup)
add_test(NAME transport_open_gate COMMAND transport_inbound open_gate)
add_test(NAME transport_s3_gate_race COMMAND transport_inbound s3_gate_race)
set_tests_properties(transport_cancel_block transport_admission_orders transport_open_gate transport_s3_gate_race
    transport_retired_open transport_retired_open_default transport_p4_call_selector transport_p4_pending_h5_cleanup
    PROPERTIES LABELS xiaotai_transport TIMEOUT 120)

# Compile against every header ABI and TLS capability in use. The official
# 2.6.3 packages of both ESP-IDF lines ship byte-identical headers, kept once in
# include/ (the archives are downloaded, never stored); `http` and `tls` build
# that set without and with TLS. These tests define only typed SDK doubles,
# never link an archive or the scripted peer.
set(tirtc_fixture_root "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/tirtc_sdk")
# The Mini package ships only with the products that use it; its tests run where it is present.
set(tirtc_abis http tls)
if(EXISTS "${tirtc_fixture_root}/variants/xmini/include")
    set(tirtc_mini TRUE)
    list(APPEND tirtc_abis mini mini_retirement)
else()
    set(tirtc_mini FALSE)
endif()
foreach(abi IN LISTS tirtc_abis)
    if(abi MATCHES "^mini(_retirement)?$")
        set(tirtc_headers "variants/xmini/include")
    else()
        set(tirtc_headers "include")
    endif()
    if(abi STREQUAL "tls" OR abi MATCHES "^mini(_retirement)?$")
        set(ssl 1)
    else()
        set(ssl 0)
    endif()
    set(test "transport_tirtc_options_${abi}")
    xiaotai_host_test(${test}
        SOURCES fake_tirtc/backend_test.c ../esp-idf/tirtc_options.c ../esp-idf/tirtc_backend.c ../esp-idf/tirtc_log.c
        INCLUDE_ONLY xiaotai_transport INCLUDE ../src "${tirtc_fixture_root}/${tirtc_headers}")
    target_compile_definitions(${test} PRIVATE "TIRTC_SDK_SSL_SUPPORT=${ssl}")
    if(abi STREQUAL "mini_retirement")
        target_compile_definitions(${test} PRIVATE XIAOTAI_TIRTC_PUBLISHED_RETIREMENT_BARRIER=1)
    endif()
    if(TEST ${test})
        add_test(NAME transport_tirtc_backend_${abi} COMMAND ${test} backend)
        set_tests_properties(transport_tirtc_backend_${abi} PROPERTIES LABELS xiaotai_transport TIMEOUT 10)
        if(abi STREQUAL "tls" OR abi STREQUAL "http")
            target_compile_definitions(${test} PRIVATE XIAOTAI_TEST_S3_JPEG=1)
            add_test(NAME transport_tirtc_h5_jpeg_${abi} COMMAND ${test} h5_jpeg)
            set_tests_properties(transport_tirtc_h5_jpeg_${abi} PROPERTIES LABELS xiaotai_transport TIMEOUT 10)
        endif()
        if(abi STREQUAL "http")
            # The official S3 packages are HTTP-only: AI vision runs on this header set.
            add_test(NAME transport_tirtc_ai_jpeg_http COMMAND ${test} ai_jpeg)
            set_tests_properties(transport_tirtc_ai_jpeg_http PROPERTIES LABELS xiaotai_transport TIMEOUT 10)
        endif()
    endif()
endforeach()

if(tirtc_mini)
    xiaotai_host_test(transport_mini_cancel SOURCES fake_tirtc/backend_test.c fakes.c
                      ../esp-idf/tirtc_options.c ../esp-idf/tirtc_backend.c ../esp-idf/tirtc_log.c
                      LINK xiaotai_transport INCLUDE ../src "${tirtc_fixture_root}/variants/xmini/include")
    target_compile_definitions(transport_mini_cancel PRIVATE XIAOTAI_TEST_MINI_CANCEL=1
                               XIAOTAI_TIRTC_EXPECT_PENDING_CLOSE=1 TIRTC_SDK_SSL_SUPPORT=1)

    foreach(test IN ITEMS transport_idle_stack transport_idle_stack_compact)
        xiaotai_host_test(${test} SOURCES fake_tirtc/idle_stack_test.c fake_tirtc/idle_stack_kernel.c
                          ../esp-idf/tirtc_idle_stack.c INCLUDE_ONLY xiaotai_transport
                          INCLUDE ../src ../esp-idf fake_freertos "${tirtc_fixture_root}/variants/xmini/include")
        target_compile_definitions(${test} PRIVATE XIAOTAI_TIRTC_IDLE_STACK_POLICY=1 TIRTC_SDK_SSL_SUPPORT=1)
        target_link_options(${test} PRIVATE
            "LINKER:--wrap=xTaskCreatePinnedToCore,--defsym=__xt_idle_stack_origin_first=__start_sdk_idle_caller,--defsym=__xt_idle_stack_origin_limit=__stop_sdk_idle_caller")
    endforeach()
    target_compile_definitions(transport_idle_stack_compact PRIVATE CONFIG_XIAOTAI_TIRTC_IDLE_STACK_COMPACT=1)
endif()

xiaotai_host_test(transport_session_stack SOURCES fake_tirtc/session_stack_test.c fake_tirtc/session_stack_creator.c
                  ../esp-idf/tirtc_session_stack.c INCLUDE_ONLY xiaotai_transport)
target_compile_definitions(transport_session_stack PRIVATE CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=1)
target_link_options(transport_session_stack PRIVATE
    "LINKER:--wrap=rtc_thread_create,--defsym=_xt_session_inbound_start=__start_fixture_in,--defsym=_xt_session_inbound_end=__stop_fixture_in,--defsym=_xt_session_outbound_start=__start_fixture_out,--defsym=_xt_session_outbound_end=__stop_fixture_out")

xiaotai_host_test(transport_input_lifetime SOURCES wp6_test.c fakes.c LINK xiaotai_transport INCLUDE ../src)
if(TEST transport_input_lifetime)
    add_test(NAME transport_input_one_slot COMMAND transport_input_lifetime one_slot)
    add_test(NAME transport_link_results COMMAND transport_input_lifetime results)
    add_test(NAME transport_input_release_strict COMMAND transport_input_lifetime release_strict)
    add_test(NAME transport_input_release_tolerated COMMAND transport_input_lifetime release_tolerated)
    set_tests_properties(transport_input_one_slot transport_link_results transport_input_release_strict transport_input_release_tolerated
                         PROPERTIES LABELS xiaotai_transport TIMEOUT 20)
endif()

xiaotai_host_test(transport_service SOURCES service_test.c fakes.c LINK xiaotai_transport INCLUDE ../src)
xiaotai_host_test(transport_diag SOURCES diag_test.c fakes.c LINK xiaotai_transport INCLUDE ../src)
add_library(transport_mini_diag_fixture STATIC EXCLUDE_FROM_ALL ../esp-idf/tirtc_diag_mini.c)
target_include_directories(transport_mini_diag_fixture PRIVATE ../src)
target_link_libraries(transport_mini_diag_fixture PRIVATE xiaotai_framework xiaotai_port xiaotai_at)
xiaotai_host_test(transport_diag_mini SOURCES diag_test.c fakes.c ../src/diagnostics.c
                  LINK xiaotai_transport transport_mini_diag_fixture INCLUDE ../src ../esp-idf/include)
target_compile_definitions(transport_diag_mini PRIVATE XIAOTAI_TRANSPORT_TGTRP_MINI=1)
if(TEST transport_diag_mini AND tirtc_mini)
    add_test(NAME transport_diag_mini_contract COMMAND "${CMAKE_COMMAND}"
             "-Dmini_contract=${tirtc_fixture_root}/variants/xmini/lib/contract.json"
             "-Dgeneric_contract=${tirtc_fixture_root}/contracts/6.0/esp32s3.json"
             "-Dfixture_dir=${CMAKE_CURRENT_BINARY_DIR}" -P "${CMAKE_CURRENT_LIST_DIR}/mini_contract_test.cmake")
    set_tests_properties(transport_diag_mini_contract PROPERTIES LABELS xiaotai_transport TIMEOUT 10)
endif()
xiaotai_host_test(transport_memtrace SOURCES memtrace_test.c ../src/memtrace.c ../esp-idf/tirtc_memtrace.c
                  LINK xiaotai_framework xiaotai_port INCLUDE_ONLY xiaotai_transport INCLUDE ../src)
target_link_options(transport_memtrace PRIVATE "LINKER:--wrap=xiaotai_mem_stats")
add_test(NAME transport_tirtc_log COMMAND transport_tirtc_options_http log)
set_tests_properties(transport_tirtc_log PROPERTIES LABELS xiaotai_transport TIMEOUT 10)
xiaotai_host_test(transport_links SOURCES links_test.c fakes.c LINK xiaotai_transport INCLUDE ../src)
add_test(NAME transport_events COMMAND transport_links events)
add_test(NAME transport_finish_opening COMMAND transport_links finish_opening)
set_tests_properties(transport_events transport_finish_opening PROPERTIES LABELS xiaotai_transport TIMEOUT 30)

xiaotai_host_test(transport_send SOURCES wp3_test.c fakes.c LINK xiaotai_transport INCLUDE ../src TIMEOUT 30)
target_link_options(transport_send PRIVATE "LINKER:--wrap=xiaotai_mem_has_external,--wrap=xiaotai_mem_alloc")
foreach(group receive subscribe_retired receive_video_peer_streams commands commands_on_demand commands_hybrid video h5_jpeg ai_jpeg audio_profiles audio_unsubscribe_permit inbound_handoff
              worker_reset worker_policy)
    add_test(NAME transport_${group} COMMAND transport_send ${group})
    set_tests_properties(transport_${group} PROPERTIES LABELS xiaotai_transport TIMEOUT 30)
endforeach()
