# SPDX-License-Identifier: MIT
# XiaoTai network engine: the link state machine with its retry policy, the
# stored credentials and history, provisioning (one setup access point with one
# page and one captive name server, one serial path), the maintenance gate and
# the diagnostics; the same for every product. It drives a driver per network
# kind (include/xiaotai/net_driver.h) and the bring-up hooks of the board's
# network part.
#
# Implemented (portable, both branches share the list):
#   src/net.c        lifecycle, worker, queue, deadlines, call slot, bring-up
#   src/state.c      snapshots, counters, address generation, gate storage
#   src/config.c     product configuration: checks and defaults
#   src/drivers.c    driver registry and binding
#   src/link.c src/backoff.c  live link state and retry policy
#   src/maintenance.c        configuration leases and orderly restart request
#   src/store.c src/history.c src/scan.c  credentials and network list
#   src/provision.c  provisioning admission, verification and durable commit
#   src/trace.c src/probe.c src/trace_at.c  packet path and bounded local probe
# Internal headers: src/internal.h (units and their interfaces),
# src/net_platform.h (the platform seam; esp-idf/ and host/ implement it).
#
# Host tests supply in-memory HTTP and UDP ports; ordinary net tests keep
# their explicitly registered providers.
set(sources src/net.c src/state.c src/config.c src/drivers.c src/wifi_policy.c src/dns_fallback.c
            src/link.c src/backoff.c src/maintenance.c src/store.c src/history.c src/scan.c src/provision.c
            src/trace.c src/probe.c src/trace_at.c src/coprocessor.c src/companion_at.c
            src/serial.c src/serial_forget.c src/diagnostics.c)
set(requires xiaotai_port xiaotai_framework xiaotai_hal xiaotai_eventlog xiaotai_at)
set(portal src/captive_dns.c src/portal.c src/portal_json.c)
if(ESP_PLATFORM)
    set(native esp-idf/wifi_driver.c esp-idf/udp.c esp-idf/probe.c)
    set(private esp_wifi esp_netif esp_event lwip)
    if(CONFIG_XIAOTAI_NET_PATTERN_PROBE)
        list(APPEND sources src/probe_v2.c)
    endif()
    # Only the V3 entry can select these hooks, after removal of the old wrappers.
    if(XIAOTAI_PORT_V3_PRODUCT AND CONFIG_XIAOTAI_NET_PATH_TRACE)
        set(trace_wrap ON)
        list(APPEND native esp-idf/trace_wrap.c)
    endif()
    if(CMAKE_BUILD_EARLY_EXPANSION OR CONFIG_XIAOTAI_NET_PROVISION_PORTAL)
        list(APPEND private esp_http_server)
    endif()
    if(CONFIG_XIAOTAI_NET_PROVISION_PORTAL)
        list(APPEND sources ${portal})
        list(APPEND native esp-idf/portal_server.c esp-idf/page.c)
    endif()
    idf_component_register(SRCS ${sources} ${native}
                           INCLUDE_DIRS "include"
                           PRIV_INCLUDE_DIRS "src"
                           REQUIRES ${requires}
                           PRIV_REQUIRES ${private})
    if(trace_wrap)
        target_link_options(${COMPONENT_LIB} INTERFACE "LINKER:--wrap=esp_netif_receive"
            "LINKER:--wrap=lwip_recvfrom" "LINKER:--wrap=lwip_socket" "LINKER:--wrap=lwip_close")
    endif()
    if(CONFIG_XIAOTAI_NET_PROVISION_PORTAL)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE XIAOTAI_NET_PORTAL_BUILTIN=1)
        target_add_binary_data(${COMPONENT_LIB} "web/setup.html" BINARY)
    endif()
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
add_library(xiaotai_net STATIC ${sources} ${portal} host/net_host.c host/probe.c)
target_include_directories(xiaotai_net PUBLIC include host/include PRIVATE src)
target_link_libraries(xiaotai_net PUBLIC ${requires})
target_compile_features(xiaotai_net PUBLIC c_std_11)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
