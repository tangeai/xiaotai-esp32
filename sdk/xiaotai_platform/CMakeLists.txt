# SPDX-License-Identifier: MIT
# XiaoTai platform core: clock gate, service discovery, device login and token,
# signaling dispatch by type, the HTTP request pool and transfers, the device
# profile and the stored environment. Portable: HTTP, signaling, clock and
# signing come from the port layer; it never touches a real-time link.
# It follows the network engine's link events (xiaotai/net.h), records its state
# changes in the event log and registers its commands with the console engine.
# Nothing differs between ESP-IDF and the host, so there are no esp-idf/ or
# host/ directories. sdkconfig.defaults.psram (signaling client memory on
# boards with external RAM) is merged by the product build, not read here.
# Build options: Kconfig, mapped in include/xiaotai/platform_config.h.
#
# Sources so far (work package P-1): src/url.c (address admission),
# src/json_scan.c (strict business code and signaling envelope, on the JSON
# library), src/environment.c (the stored environment base, through the
# storage port's worker). P-2: src/platform.c src/worker.c
# src/request_pool.c src/gate.c (requests and queued transfer execution).
# P-3: src/session.c src/discovery.c src/login.c src/token.c.
# P-4: src/signaling.c, src/signaling_hold.c, src/signal_pool.c, src/signal_scan.c.
# P-5: src/transfer.c src/binding.c src/guest.c.
# P-6: src/profile.c (with xiaotai_platform_scene_parts), diagnostics;
# src/at_reconnect.c only when
# XIAOTAI_PLATFORM_RECONNECT_COMMAND is set (handlers prefixed
# xiaotai_platform_at_dev_, checked after linking).
set(sources src/url.c src/json_scan.c src/environment.c src/platform.c src/worker.c src/request_pool.c src/gate.c
    src/session.c src/discovery.c src/login.c src/token.c src/signaling.c src/signaling_hold.c
    src/signal_pool.c src/signal_scan.c src/transfer.c src/binding.c src/guest.c src/profile.c src/diagnostics.c)
set(requires xiaotai_port xiaotai_json xiaotai_framework xiaotai_identity xiaotai_net xiaotai_eventlog xiaotai_at)
if(ESP_PLATFORM)
    if(CONFIG_XIAOTAI_PLATFORM_RECONNECT_COMMAND)
        list(APPEND sources src/at_reconnect.c)
    endif()
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src" REQUIRES ${requires})
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
add_library(xiaotai_platform STATIC ${sources})
target_include_directories(xiaotai_platform PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_platform PUBLIC ${requires})
target_compile_definitions(xiaotai_platform PRIVATE XIAOTAI_PLATFORM_MQTT_STOP_FREES_WORKER=1)
if(CONFIG_XIAOTAI_PLATFORM_RECONNECT_COMMAND)
    target_sources(xiaotai_platform PRIVATE src/at_reconnect.c)
    target_compile_definitions(xiaotai_platform PUBLIC CONFIG_XIAOTAI_PLATFORM_RECONNECT_COMMAND=1)
endif()
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
