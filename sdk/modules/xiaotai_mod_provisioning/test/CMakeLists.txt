# SPDX-License-Identifier: MIT
# Host tests of the provisioning module. Each test builds the module from its
# sources with the build options it needs (-DCONFIG_...) against the real
# framework, event log and port, and fakes of the network engine, the
# preferences pending flag, the AT verb tables (fake_net.c) and the platform
# gate (fake_platform.c, left out where the platform core is not in the
# build). The settle bound is lowered to 600 ms so it is measured on an
# absolute schedule; mod_provisioning_config checks the defaults.
set(src "${CMAKE_CURRENT_SOURCE_DIR}/../src")
set(core "${src}/provisioning.c" "${src}/provisioning_begin.c" "${src}/provisioning_at.c")
set(harness harness.c fake_net.c)

# The manifest's "commands" as a C table, so mod_provisioning_at compares what
# the module registers (what AT+HELP prints) with the manifest itself.
set(manifest "${CMAKE_CURRENT_SOURCE_DIR}/../module.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${manifest}")
file(READ "${manifest}" manifest_text)
string(JSON command_count LENGTH "${manifest_text}" commands)
set(rows "")
math(EXPR command_last "${command_count} - 1")
foreach(i RANGE ${command_last})
    string(JSON entry GET "${manifest_text}" commands ${i})
    set(fields "")
    foreach(field command verb usage option)
        string(JSON value ERROR_VARIABLE missing GET "${entry}" ${field})
        if(missing)
            set(value "")
        endif()
        string(APPEND fields "\"${value}\", ")
    endforeach()
    string(APPEND rows "    {${fields}},\n")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/manifest_commands.h" CONTENT
"// SPDX-License-Identifier: MIT
// Generated from module.json by test/CMakeLists.txt.
#pragma once
#include <stddef.h>
typedef struct {
    const char *command, *verb, *usage, *option;
} manifest_command_t;
static const manifest_command_t manifest_commands[] = {
@rows@};
#define MANIFEST_COMMAND_COUNT (sizeof(manifest_commands) / sizeof(manifest_commands[0]))
" @ONLY)

function(provisioning_test name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;DEFINES")
    xiaotai_host_test(${name} SOURCES ${arg_SOURCES}
                      LINK xiaotai_framework xiaotai_eventlog xiaotai_port
                      INCLUDE_ONLY xiaotai_mod_provisioning xiaotai_platform
                      INCLUDE "${src}" "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}"
                      TIMEOUT 120)
    target_compile_definitions(${name} PRIVATE ${arg_DEFINES})
endfunction()

set(with_platform ${core} "${src}/with_platform.c" "${src}/provisioning_at_forget.c" ${harness} fake_platform.c)
set(defaults CONFIG_XIAOTAI_PROVISIONING_SETTLE_WAIT_MS=600 CONFIG_XIAOTAI_NET_CONSOLE_FORGET=1)

provisioning_test(mod_provisioning_phase SOURCES phase_test.c ${with_platform} DEFINES ${defaults})
provisioning_test(mod_provisioning_begin SOURCES begin_test.c ${with_platform} DEFINES ${defaults})
provisioning_test(mod_provisioning_intent SOURCES intent_test.c ${with_platform} DEFINES ${defaults})
provisioning_test(mod_provisioning_hold SOURCES hold_test.c ${with_platform} DEFINES ${defaults})
provisioning_test(mod_provisioning_restart SOURCES restart_test.c ${with_platform} DEFINES ${defaults})
provisioning_test(mod_provisioning_s3_p4_direct SOURCES direct_link_test.c ${with_platform}
                  DEFINES ${defaults} CONFIG_XIAOTAI_PROVISIONING_LOCAL_POLICY=1)
provisioning_test(mod_provisioning_mz28_direct SOURCES direct_restart_test.c ${with_platform}
                  DEFINES ${defaults} CONFIG_XIAOTAI_PROVISIONING_LOCAL_POLICY=2)
provisioning_test(mod_provisioning_at SOURCES at_test.c ${with_platform} DEFINES ${defaults})
# The option off: the FORGET source is not even compiled.
provisioning_test(mod_provisioning_at_off
                  SOURCES at_test.c ${core} "${src}/with_platform.c" ${harness} fake_platform.c
                  DEFINES CONFIG_XIAOTAI_PROVISIONING_SETTLE_WAIT_MS=600 CONFIG_XIAOTAI_NET_CONSOLE_FORGET=0)
# Without the platform core: platform_none.c, and no platform symbol linked.
provisioning_test(mod_provisioning_no_platform
                  SOURCES no_platform_test.c ${core} "${src}/platform_none.c" "${src}/provisioning_at_forget.c"
                          ${harness}
                  DEFINES ${defaults})
# Product options: held begin, no bound, held restart after a setting failure.
provisioning_test(mod_provisioning_options_hold SOURCES options_hold_test.c ${with_platform}
                  DEFINES CONFIG_XIAOTAI_PROVISIONING_SETTLE_WAIT_MS=0 CONFIG_XIAOTAI_PROVISIONING_BEGIN_FAILURE=1
                          CONFIG_XIAOTAI_PROVISIONING_STORE_SETTINGS_HOLD=1 CONFIG_XIAOTAI_NET_CONSOLE_FORGET=1)
# The same orderly exclusive begin with and without the product's wait override.
foreach(settle default unbounded)
    set(target mod_provisioning_settle_${settle})
    provisioning_test(${target} SOURCES settle_window_test.c ${with_platform}
                      DEFINES CONFIG_XIAOTAI_PROVISIONING_BEGIN_FAILURE=1)
    if(settle STREQUAL "unbounded")
        target_compile_definitions(${target} PRIVATE CONFIG_XIAOTAI_PROVISIONING_SETTLE_WAIT_MS=0)
    endif()
    target_link_options(${target} PRIVATE "LINKER:--wrap=xiaotai_time_us")
endforeach()
# Product options: continue after settings failures, store again on a period.
provisioning_test(mod_provisioning_options_continue SOURCES options_continue_test.c ${with_platform}
                  DEFINES CONFIG_XIAOTAI_PROVISIONING_SETTLE_WAIT_MS=600 CONFIG_XIAOTAI_PROVISIONING_BEGIN_FAILURE=2
                          CONFIG_XIAOTAI_PROVISIONING_STORE_RETRY_MS=300 CONFIG_XIAOTAI_NET_CONSOLE_FORGET=1)

xiaotai_host_test(mod_provisioning_config SOURCES config_test.c INCLUDE_ONLY xiaotai_mod_provisioning)
target_compile_definitions(mod_provisioning_config PRIVATE
                           XIAOTAI_TEST_KCONFIG="${CMAKE_CURRENT_SOURCE_DIR}/../Kconfig")
