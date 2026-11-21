# SPDX-License-Identifier: MIT
# XiaoTai transport engine: links to a peer over the RTC stack (xiaotai/link.h)
# and the engine that runs that stack (xiaotai/transport.h). The portable core
# (src/) owns links, generations, profiles, gates, pools and events; the RTC
# package is reached only through src/transport_backend.h, implemented in
# esp-idf/ (TiRTC) and in host/ (a scripted peer for tests). src/internal.h is
# the core's internal interface between its files.
#
# Portable sources, by work package of the transport specification:
#   WP1  src/transport.c src/run_control.c
#   WP2  src/links.c src/inbound.c src/events.c
#   WP3  src/send.c src/receive.c src/commands.c
#   WP5  src/service.c src/diagnostics.c and src/memtrace.c (memtrace.c only with
#        CONFIG_XIAOTAI_TRANSPORT_MEMTRACE on ESP-IDF: it holds the AT+MEMTRACE
#        handler, whose names start with xiaotai_transport_memtrace, the
#        "symbol" of engine.json that the post-link check looks for; host tests
#        compile it always)
# Platform sources:
#   esp-idf/   clock.c (WP1); tirtc_backend.c tirtc_options.c tirtc_log.c (WP4);
#              tirtc_diag_mini.c (WP5, only for a locked Mini package);
#              tirtc_memtrace.c (WP5, only with CONFIG_XIAOTAI_TRANSPORT_MEMTRACE)
#   host/      script_backend.c (scripted backend and manual clock),
#              include/xiaotai_transport_host.h
set(requires xiaotai_port xiaotai_framework xiaotai_identity xiaotai_net xiaotai_platform xiaotai_media
             xiaotai_eventlog xiaotai_at)
set(sources src/transport.c src/run_control.c src/links.c src/inbound.c src/events.c
            src/send.c src/receive.c src/commands.c src/diagnostics.c src/service.c)
if(ESP_PLATFORM)
    # The TiRTC package enters the build only through this component, so a
    # product without real-time modules links no RTC stack. Which package is
    # built is chosen by the build entry per chip and ESP-IDF line
    # (components/tirtc_sdk), never here. It is a private requirement: the
    # public headers never name the package, so components that require the
    # transport do not inherit its include paths or link interface; only the
    # esp-idf/ backend includes it.
    idf_component_register(SRCS ${sources} "esp-idf/clock.c" "esp-idf/tirtc_backend.c"
                           "esp-idf/tirtc_options.c" "esp-idf/tirtc_log.c"
                           INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src"
                           REQUIRES ${requires} PRIV_REQUIRES tirtc_sdk)
    if(CMAKE_BUILD_EARLY_EXPANSION)
        return()
    endif()
    idf_component_get_property(tirtc_component_dir tirtc_sdk COMPONENT_DIR)
    include("${tirtc_component_dir}/select_variant.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/esp-idf/verify_mini_contract.cmake")
    set(xt_diag_header "${CMAKE_CURRENT_LIST_DIR}/esp-idf/include/tgtrp_internal_diag.h")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${tirtc_contract_file}")
    xiaotai_tirtc_verify_mini_contract("${tirtc_contract_file}" "${tirtc_package_name}" "${xt_diag_header}" xt_tirtc_mini)
    if(xt_tirtc_mini)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${xt_diag_header}")
        target_sources(${COMPONENT_LIB} PRIVATE esp-idf/tirtc_diag_mini.c)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE XIAOTAI_TIRTC_EXPECT_PENDING_CLOSE=1 XIAOTAI_TRANSPORT_TGTRP_MINI=1)
    endif()
    # This policy is private to the ESP port and only the exact audited input.
    # A new package or port takes the ordinary TiRtcStart path.
    set(xt_idle_stack_linked OFF)
    if(NOT CONFIG_SPIRAM AND CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY
       AND "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}" STREQUAL "6.0.2")
        set(xt_idle_stack_archive "${sdk_dir}/libTiRTC.a")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${xt_idle_stack_archive}")
        file(SHA256 "${xt_idle_stack_archive}" xt_idle_stack_hash)
        if(xt_idle_stack_hash STREQUAL "328dfaaa3fb313216f30734cbfd1cdec676fd44cabfd8adcfa3b8a3462ef45f3")
            set(xt_idle_stack_linked ON)
            target_sources(${COMPONENT_LIB} PRIVATE esp-idf/tirtc_idle_stack.c)
            target_compile_definitions(${COMPONENT_LIB} PRIVATE XIAOTAI_TIRTC_IDLE_STACK_POLICY=1)
            if(xt_tirtc_mini AND IDF_TARGET STREQUAL "esp32c3")
                # Only this pinned binary counts published retirement under its
                # connection lock, through object free, in PendingConnections.
                target_compile_definitions(${COMPONENT_LIB} PRIVATE XIAOTAI_TIRTC_PUBLISHED_RETIREMENT_BARRIER=1)
            endif()
            target_link_libraries(${COMPONENT_LIB} INTERFACE
                "-Wl,-u,__wrap_xTaskCreatePinnedToCore,--wrap=xTaskCreatePinnedToCore,--defsym=__xt_idle_stack_origin_first=freertos_ThreadCreateWithStackSize,--defsym=__xt_idle_stack_origin_limit=freertos_ThreadCreateWithStackSize+0xc0")
            idf_build_set_property(XIAOTAI_TIRTC_IDLE_STACK_ARCHIVE "${xt_idle_stack_archive}")
            if(xt_tirtc_mini AND CONFIG_XIAOTAI_TIRTC_SESSION_STACK_9216)
                # Make the prebuilt archive visible to IDF's symbol-level ldgen
                # when it enumerates the dependencies of this component.
                target_link_libraries(${COMPONENT_LIB} PRIVATE tirtc_prebuilt)
                __ldgen_add_fragment_files("${CMAKE_CURRENT_LIST_DIR}/esp-idf/tirtc_session_stack.lf")
                target_sources(${COMPONENT_LIB} PRIVATE esp-idf/tirtc_session_stack.c)
                target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,-u,__wrap_rtc_thread_create,--wrap=rtc_thread_create")
                idf_build_set_property(XIAOTAI_TIRTC_SESSION_STACK_POLICY ON)
            endif()
        endif()
    endif()
    if(CONFIG_XIAOTAI_TIRTC_IDLE_STACK_COMPACT AND
       (NOT xt_idle_stack_linked OR NOT xt_tirtc_mini OR NOT tirtc_package STREQUAL "6.0/xmini-mini"
        OR NOT IDF_TARGET STREQUAL "esp32c3" OR NOT CONFIG_FREERTOS_UNICORE))
        message(FATAL_ERROR "TIRTC_IDLE_STACK_COMPACT requires the audited ESP32-C3 Mini archive, IDF 6.0.2, "
            "single-core FreeRTOS canary and no external RAM")
    endif()
    if(CONFIG_XIAOTAI_TRANSPORT_MEMTRACE)
        # Pull the wrapper object out of the archive even when the linker
        # finds TiRTC's first allocator reference after scanning transport.
        target_sources(${COMPONENT_LIB} PRIVATE src/memtrace.c esp-idf/tirtc_memtrace.c)
        target_link_libraries(${COMPONENT_LIB} INTERFACE
            "-Wl,-u,__wrap_Mx_malloc,--wrap=Mx_malloc,--wrap=Mx_calloc,--wrap=Mx_realloc,--wrap=Mx_free")
    endif()
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
add_library(xiaotai_transport STATIC ${sources} host/script_backend.c)
if(XIAOTAI_HOST_PRODUCT_SELECTION AND CONFIG_XIAOTAI_TRANSPORT_MEMTRACE)
    target_sources(xiaotai_transport PRIVATE src/memtrace.c)
endif()
target_include_directories(xiaotai_transport PUBLIC include host/include PRIVATE src)
target_link_libraries(xiaotai_transport PUBLIC ${requires})
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
