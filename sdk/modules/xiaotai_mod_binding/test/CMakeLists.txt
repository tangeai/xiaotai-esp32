# SPDX-License-Identifier: MIT
# Host tests of the binding module (work package MB-1). Each test compiles the
# module's sources itself with the fakes of fake_platform.c: the platform
# core, the identity component and the network engine are replaced
# (INCLUDE_ONLY), the port, framework, event log and JSON library are real.
# Linked with --wrap: the clocks (fake unix time and manual monotonic time for
# the window group), the attempt context's
# allocation and release (memory kind, wiped before release) and the owner
# queue (a full queue on request). binding_flow_test.c holds every group;
# BINDING_GROUP selects one per executable.
set(binding_sources ../src/binding.c ../src/binding_report.c ../src/binding_grant.c ../src/binding_window.c
                    ../src/binding_reset.c ../src/binding_at.c)
get_filename_component(binding_dir "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

function(binding_test group)
    cmake_parse_arguments(PARSE_ARGV 1 arg "REAL_JWT" "TIMEOUT" "")
    if(NOT arg_TIMEOUT)
        set(arg_TIMEOUT 120)
    endif()
    set(name mod_binding_${group})
    if(arg_REAL_JWT)
        set(platform_parser xiaotai_platform)
    endif()
    xiaotai_host_test(${name}
        SOURCES binding_flow_test.c fake_platform.c ${binding_sources} ../src/announce_none.c
        INCLUDE ../src
        INCLUDE_ONLY xiaotai_mod_binding
        LINK xiaotai_port xiaotai_framework xiaotai_eventlog xiaotai_json xiaotai_at ${platform_parser}
        TIMEOUT ${arg_TIMEOUT})
    target_compile_definitions(${name} PRIVATE BINDING_GROUP=${group}
                               "BINDING_COMPONENT_DIR=\"${binding_dir}\"")
    if(arg_REAL_JWT)
        target_compile_definitions(${name} PRIVATE BINDING_REAL_JWT)
    endif()
    target_link_options(${name} PRIVATE
        "LINKER:--wrap=xiaotai_clock_unix_ms,--wrap=xiaotai_time_us,--wrap=xiaotai_mem_calloc,--wrap=xiaotai_mem_free,--wrap=xiaotai_run")
endfunction()

binding_test(first)
binding_test(prepare)
binding_test(resign)
binding_test(grant)
binding_test(window)
binding_test(window_jwt REAL_JWT)
binding_test(cancel)
binding_test(ack_lost)
binding_test(quiesce_guard)
binding_test(auto_start)

xiaotai_host_test(mod_binding_window_resume
    SOURCES binding_reconnect_test.c fake_platform.c ${binding_sources} ../src/announce_none.c
    INCLUDE ../src
    INCLUDE_ONLY xiaotai_mod_binding
    LINK xiaotai_port xiaotai_framework xiaotai_eventlog xiaotai_json xiaotai_at)
target_link_options(mod_binding_window_resume PRIVATE
    "LINKER:--wrap=xiaotai_clock_unix_ms,--wrap=xiaotai_time_us,--wrap=xiaotai_mem_calloc,--wrap=xiaotai_mem_free,--wrap=xiaotai_run")

xiaotai_host_test(mod_binding_announce
    SOURCES binding_announce_test.c fake_media.c fake_platform.c ${binding_sources} ../src/binding_announce.c
    INCLUDE ../src
    INCLUDE_ONLY xiaotai_mod_binding
    LINK xiaotai_port xiaotai_framework xiaotai_eventlog xiaotai_json xiaotai_sounds xiaotai_at)
target_link_options(mod_binding_announce PRIVATE
    "LINKER:--wrap=xiaotai_clock_unix_ms,--wrap=xiaotai_time_us,--wrap=xiaotai_mem_calloc,--wrap=xiaotai_mem_free,--wrap=xiaotai_run")

# Compare the real verb table used by AT+HELP to the selected manifest rows.
set(manifest "${binding_dir}/module.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${manifest}")
file(READ "${manifest}" manifest_text)
string(JSON last LENGTH "${manifest_text}" commands)
math(EXPR last "${last} - 1")
set(rows "")
foreach(i RANGE ${last})
    set(fields "")
    foreach(field command verb usage option)
        string(JSON value ERROR_VARIABLE missing GET "${manifest_text}" commands ${i} ${field})
        if(missing)
            set(value "")
        endif()
        string(APPEND fields "\"${value}\", ")
    endforeach()
    string(APPEND rows "    {${fields}},\n")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/manifest_commands.h" CONTENT
"// Generated from module.json.
static const struct { const char *command, *verb, *usage, *option; } manifest_commands[] = {
@rows@};
" @ONLY)

foreach(group IN ITEMS at at_off forget intent_local direct_reset)
    set(name mod_binding_${group})
    xiaotai_host_test(${name}
        SOURCES binding_at_test.c fake_platform.c ${binding_sources} ../src/announce_none.c ../src/binding_at_dev.c
        INCLUDE ../src ../../../xiaotai_at/src "${CMAKE_CURRENT_BINARY_DIR}"
        INCLUDE_ONLY xiaotai_mod_binding
        LINK xiaotai_port xiaotai_framework xiaotai_eventlog xiaotai_json xiaotai_at)
    target_compile_definitions(${name} PRIVATE BINDING_AT_GROUP=${group})
    if(group STREQUAL "at")
        target_compile_definitions(${name} PRIVATE CONFIG_XIAOTAI_BINDING_DEV_COMMANDS=1)
    endif()
    target_link_options(${name} PRIVATE
        "LINKER:--wrap=xiaotai_clock_unix_ms,--wrap=xiaotai_time_us,--wrap=xiaotai_mem_calloc,--wrap=xiaotai_mem_free,--wrap=xiaotai_run,--wrap=xiaotai_at_fail,--wrap=xiaotai_identity_wipe,--wrap=xiaotai_signal_take"
        "LINKER:--wrap=xiaotai_at_stream")
endforeach()
