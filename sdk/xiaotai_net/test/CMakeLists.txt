# SPDX-License-Identifier: MIT
# Host tests of the network engine.
#
# net_product_glue compiles the product glue that scripts/sdk_selection.py
# generates from this engine's manifest (net_glue.py writes it at build time,
# so a change to engine.json or to the generator is picked up) and checks what
# the engine's real xiaotai_net_configure keeps; only src/config.c is linked
# in the end, as in a product the glue runs before the engine exists.
#
# net_lifecycle runs the engine with the real framework, the host port and
# the scripted driver of host/net_host.c; the board's network parts are the
# test's own xiaotai_hal_network(). It wraps the port's allocators and task
# creation to see where the engine puts its memory and its worker.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
get_filename_component(tree "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(glue_dir "${CMAKE_CURRENT_BINARY_DIR}/glue")
set(glue_sources "${glue_dir}/net_glue_fixed_table.c" "${glue_dir}/net_glue_doubling.c"
                 "${glue_dir}/net_glue_plain_serial.c" "${glue_dir}/net_glue_szpi.c"
                 "${glue_dir}/net_glue_cores3.c" "${glue_dir}/net_glue_waveshare.c"
                 "${glue_dir}/net_glue_wt99.c")
add_custom_command(
    OUTPUT ${glue_sources}
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/net_glue.py" "${tree}" "${glue_dir}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/net_glue.py" "${CMAKE_CURRENT_SOURCE_DIR}/../engine.json"
            "${tree}/scripts/sdk_selection.py" "${tree}/products/xiaotai-szpi-s3/product.json"
            "${tree}/products/xiaotai-cores3-lite/product.json" "${tree}/products/xiaotai-waveshare-p4-rev3/product.json"
            "${tree}/products/xiaotai-wt99p461-s1-1v1/product.json"
    COMMENT "Generating the network engine's product glue"
    VERBATIM)
xiaotai_host_test(net_product_glue SOURCES product_glue_test.c ${glue_sources} LINK xiaotai_net
                  INCLUDE ../src)

xiaotai_host_test(net_lifecycle SOURCES lifecycle_test.c LINK xiaotai_net INCLUDE ../src)
target_link_options(net_lifecycle PRIVATE "LINKER:--wrap=xiaotai_mem_alloc" "LINKER:--wrap=xiaotai_mem_calloc"
                    "LINKER:--wrap=xiaotai_task_create")
if(TEST net_lifecycle)
    add_test(NAME net_owner_call_deadline COMMAND net_lifecycle owner-deadline)
    add_test(NAME net_companion_start_failure COMMAND net_lifecycle companion-start-failure)
    set_tests_properties(net_owner_call_deadline net_companion_start_failure PROPERTIES LABELS xiaotai_net TIMEOUT 15)
endif()
xiaotai_host_test(net_gate SOURCES maintenance_test.c LINK xiaotai_net INCLUDE ../src)
target_link_options(net_gate PRIVATE "LINKER:--wrap=xiaotai_restart_request" "LINKER:--wrap=net_units_maintenance_released")
if(TEST net_gate)
    foreach(scenario bringup promise queue busy release_lost takeover notice)
        add_test(NAME net_gate_${scenario} COMMAND net_gate ${scenario})
        set_tests_properties(net_gate_${scenario} PROPERTIES LABELS xiaotai_net TIMEOUT 20)
    endforeach()
endif()
xiaotai_host_test(net_dns_fallback SOURCES wifi_policy_test.c LINK xiaotai_net INCLUDE ../src)
xiaotai_host_test(net_companion SOURCES companion_test.c LINK xiaotai_net INCLUDE ../src)
target_link_options(net_companion PRIVATE "LINKER:--wrap=xiaotai_time_us")
if(TEST net_companion)
    foreach(scenario partial unavailable prepare_blocked prepare_restart rollback_release rollback_timeout
                     update_bad update_begin_fault update_fault update_ok admission_blocked admission_scan)
        add_test(NAME net_companion_${scenario} COMMAND net_companion ${scenario})
        set_tests_properties(net_companion_${scenario} PROPERTIES LABELS xiaotai_net TIMEOUT 20)
    endforeach()
endif()
xiaotai_host_test(net_companion_at SOURCES companion_at_test.c LINK xiaotai_net INCLUDE ../src)
target_link_options(net_companion_at PRIVATE "LINKER:--wrap=xiaotai_mem_has_external,--wrap=xiaotai_task_create")
if(TEST net_companion_at)
    foreach(scenario rollback offline failed success unsupported queue failed_queue
                     maintenance_info_parity maintenance_info_default maintenance_info_during_transfer
                     p4_cached_identity p4_retry_no_link p4_retry_no_link_not_timeout p4_retry_unfinished
                     p4_retry_bad_image p4_retry_unsupported p4_retry_power_cycle p4_retry_unknown
                     retry_default_unsupported p4_driver_start_failure wt99_driver_start_failure)
        add_test(NAME net_companion_at_${scenario} COMMAND net_companion_at ${scenario})
        set_tests_properties(net_companion_at_${scenario} PROPERTIES LABELS xiaotai_net TIMEOUT 20)
    endforeach()
endif()
xiaotai_host_test(net_wifi_driver SOURCES wifi_driver_test.c ../esp-idf/wifi_driver.c
                  LINK xiaotai_net INCLUDE ../src wifi_idf)
target_link_options(net_wifi_driver PRIVATE "LINKER:--wrap=xiaotai_mem_has_external")
xiaotai_host_test(net_wifi_worker SOURCES wifi_worker_test.c wifi_driver_test.c ../esp-idf/wifi_driver.c
                  LINK xiaotai_net INCLUDE ../src wifi_idf)
target_compile_definitions(net_wifi_worker PRIVATE NW11_WORKER_TEST=1)
target_link_options(net_wifi_worker PRIVATE "LINKER:--wrap=xiaotai_time_us")
if(TEST net_wifi_worker)
    add_test(NAME net_wifi_worker_join_errors COMMAND net_wifi_worker join_errors)
    set_tests_properties(net_wifi_worker net_wifi_worker_join_errors
        PROPERTIES LABELS xiaotai_net TIMEOUT 15 ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1")
endif()
foreach(family ipv6 ipv4)
    xiaotai_host_test(net_portal_platform_${family}
        SOURCES portal_platform_test.c ../esp-idf/portal_server.c ../esp-idf/udp.c ../esp-idf/page.c
        LINK xiaotai_port INCLUDE_ONLY xiaotai_net INCLUDE portal_idf ../src)
endforeach()
target_compile_definitions(net_portal_platform_ipv6 PRIVATE CONFIG_LWIP_IPV6=1)
target_compile_definitions(net_portal_platform_ipv4 PRIVATE CONFIG_LWIP_IPV6=0)

foreach(mode off path transport)
    xiaotai_host_test(net_trace_${mode}
        SOURCES net_trace_test.c ../src/trace.c ../src/trace_at.c ../src/probe.c
        LINK xiaotai_port INCLUDE_ONLY xiaotai_net INCLUDE ../src)
    target_link_options(net_trace_${mode} PRIVATE "LINKER:--wrap=xiaotai_time_us")
    if(NOT mode STREQUAL "off")
        target_compile_definitions(net_trace_${mode} PRIVATE CONFIG_XIAOTAI_NET_PATH_TRACE=1
                                   CONFIG_XIAOTAI_NET_PATH_TRACE_SECONDS=3)
        target_link_options(net_trace_${mode} PRIVATE "LINKER:--wrap=xiaotai_mem_calloc")
    endif()
endforeach()
target_compile_definitions(net_trace_transport PRIVATE CONFIG_XIAOTAI_NET_PATH_TRACE_TRANSPORT=1)

xiaotai_host_test(net_trace_pattern
    SOURCES net_trace_test.c ../src/trace.c ../src/trace_at.c ../src/probe.c ../../xiaotai_at/src/at_text.c
    LINK xiaotai_port INCLUDE_ONLY xiaotai_net INCLUDE ../src)
target_compile_definitions(net_trace_pattern PRIVATE CONFIG_XIAOTAI_NET_PATTERN_PROBE=1)

xiaotai_host_test(net_trace_native SOURCES net_trace_native_test.c ../src/probe.c ../src/probe_v2.c
    ../esp-idf/probe.c ../esp-idf/trace_wrap.c
    INCLUDE_ONLY xiaotai_net INCLUDE trace_idf ../src)
target_compile_definitions(net_trace_native PRIVATE CONFIG_XIAOTAI_NET_PATH_TRACE=1 CONFIG_XIAOTAI_NET_PATTERN_PROBE=1)

xiaotai_host_test(net_probe_v2 SOURCES probe_v2_test.c ../src/probe_v2.c INCLUDE ../src)

xiaotai_host_test(net_link SOURCES link_test.c LINK xiaotai_net INCLUDE ../src)
target_link_options(net_link PRIVATE "LINKER:--wrap=xiaotai_time_us")
if(TEST net_link)
    add_test(NAME net_backoff COMMAND net_link backoff)
    set_tests_properties(net_backoff PROPERTIES LABELS xiaotai_net TIMEOUT 15)
    add_test(NAME net_scan COMMAND net_link scan_cache)
    add_test(NAME net_scan_timeout COMMAND net_link scan_timeout)
    add_test(NAME net_scan_disconnect_timeout COMMAND net_link scan_disconnect_timeout)
    set_tests_properties(net_scan net_scan_timeout net_scan_disconnect_timeout PROPERTIES LABELS xiaotai_net TIMEOUT 15)
    foreach(case controls control_notice timeout capacity policies verification clock stopped overflow_stop ordering scan
                 mixed_retry retry_after_stop retry_immediately_after_stop outage_history)
        add_test(NAME net_link_${case} COMMAND net_link ${case})
        set_tests_properties(net_link_${case} PROPERTIES LABELS xiaotai_net TIMEOUT 15)
    endforeach()
endif()

xiaotai_host_test(net_store SOURCES store_test.c LINK xiaotai_net INCLUDE ../src ../../xiaotai_port/common)
target_link_options(net_store PRIVATE "LINKER:--wrap=xiaotai_kv_backend_set,--wrap=xiaotai_kv_backend_commit")
if(TEST net_store)
    add_test(NAME net_store_plain COMMAND net_store plain)
    add_test(NAME net_store_serial COMMAND net_store serial)
    add_test(NAME net_store_timeout COMMAND net_store timeout)
    add_test(NAME net_history COMMAND net_store history)
    add_test(NAME net_history_492 COMMAND net_store history_492)
    add_test(NAME net_store_legacy_failures COMMAND net_store legacy_failures)
    add_test(NAME net_history_legacy_failures COMMAND net_store legacy_history_failures)
    set_tests_properties(net_store_plain net_store_serial net_store_timeout net_history net_history_492
        net_store_legacy_failures net_history_legacy_failures
        PROPERTIES LABELS xiaotai_net TIMEOUT 30)
    foreach(index RANGE 0 6)
        add_test(NAME net_history_incompatible_${index} COMMAND net_store history_bad ${index})
        set_tests_properties(net_history_incompatible_${index} PROPERTIES LABELS xiaotai_net TIMEOUT 30)
    endforeach()
endif()

xiaotai_host_test(net_provision SOURCES provision_test.c LINK xiaotai_net INCLUDE ../src)
target_link_options(net_provision PRIVATE "LINKER:--wrap=xiaotai_time_us" "LINKER:--wrap=xiaotai_restart_request")
if(TEST net_provision)
    foreach(case combinations legacy recovery saved manual_disconnect store_order store_notice xmini_credentials xmini_portal_storage)
        add_test(NAME net_provision_${case} COMMAND net_provision ${case})
        set_tests_properties(net_provision_${case} PROPERTIES LABELS xiaotai_net TIMEOUT 20)
    endforeach()
endif()

xiaotai_host_test(net_dns SOURCES captive_dns_test.c LINK xiaotai_net INCLUDE ../src)
target_link_options(net_dns PRIVATE "LINKER:--wrap=xiaotai_task_create")
xiaotai_host_test(net_portal_json SOURCES portal_json_test.c LINK xiaotai_net INCLUDE ../src)
xiaotai_host_test(net_portal SOURCES portal_test.c LINK xiaotai_net INCLUDE ../src)
target_link_options(net_portal PRIVATE "LINKER:--wrap=xiaotai_restart_request" "LINKER:--wrap=net_call_cancellable")
if(TEST net_portal)
    add_test(NAME net_page COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/net_page.py)
    add_test(NAME net_portal_native_transaction COMMAND net_portal native-transaction)
    add_test(NAME net_portal_legacy_s3_credentials COMMAND net_portal legacy-s3-credentials)
    set_tests_properties(net_page net_portal_native_transaction net_portal_legacy_s3_credentials
        PROPERTIES LABELS xiaotai_net TIMEOUT 20)
endif()

xiaotai_host_test(net_serial SOURCES serial_test.c LINK xiaotai_net INCLUDE ../src)
target_link_options(net_serial PRIVATE "LINKER:--wrap=xiaotai_restart_request" "LINKER:--wrap=net_probe_platform_start"
                    "LINKER:--wrap=net_provision_submit_checked")
if(TEST net_serial)
    add_test(NAME net_diag COMMAND net_serial diag)
    set_tests_properties(net_diag PROPERTIES LABELS xiaotai_net TIMEOUT 20)
    foreach(case blocked exclusive forget xmini)
        add_test(NAME net_serial_${case} COMMAND net_serial ${case})
        set_tests_properties(net_serial_${case} PROPERTIES LABELS xiaotai_net TIMEOUT 20)
    endforeach()
endif()
