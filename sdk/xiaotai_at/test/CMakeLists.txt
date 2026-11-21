# SPDX-License-Identifier: MIT
# Host tests of the console engine. Each implementation package adds its own
# (at_text, at_verbs, at_console, at_stream, at_notify, at_builtin).
#
# at_config: the build options in Kconfig, xiaotai/at_config.h and the
# manifest's command options agree and reach the public headers.
xiaotai_host_test(at_config SOURCES at_config_test.c at_config_override.c at_config_force_only.c
                  INCLUDE_ONLY xiaotai_at)
target_compile_definitions(at_config PRIVATE XIAOTAI_TEST_KCONFIG="${CMAKE_CURRENT_SOURCE_DIR}/../Kconfig"
                                             XIAOTAI_TEST_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/../engine.json")

# at_text (AT-1): the text helpers built from source against a fake of the
# reply seam (src/internal.h), so the faults xiaotai_at_printf reports are
# observed directly.
xiaotai_host_test(at_text SOURCES at_text_test.c ../src/at_text.c
                  INCLUDE_ONLY xiaotai_at INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
# at_text_library (AT-1): the same helpers through the linked library, as
# command authors use them.
xiaotai_host_test(at_text_library SOURCES at_text_library_test.c LINK xiaotai_at)
xiaotai_host_test(at_verbs SOURCES at_verbs_test.c LINK xiaotai_at
                  INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_options(at_verbs PRIVATE "LINKER:--wrap=xiaotai_at_fail")

xiaotai_host_test(at_console SOURCES at_console_test.c LINK xiaotai_at)
target_link_options(at_console PRIVATE "LINKER:--wrap=xiaotai_task_create")
xiaotai_host_test(at_caller_owner SOURCES at_caller_owner_test.c LINK xiaotai_at)
target_link_options(at_caller_owner PRIVATE "LINKER:--wrap=xiaotai_task_create")
xiaotai_host_test(at_help SOURCES at_help_test.c LINK xiaotai_at)

xiaotai_host_test(at_stream SOURCES at_stream_test.c LINK xiaotai_at)
xiaotai_host_test(at_notify SOURCES at_notify_test.c LINK xiaotai_at INCLUDE ../src)

set(at_builtin_wrappers xiaotai_firmware xiaotai_restart xiaotai_port_report xiaotai_port_probes
                        xiaotai_port_probe xiaotai_mem_failure xiaotai_mem_rtc_stats)
function(at_builtin_link_options target)
    foreach(api IN LISTS at_builtin_wrappers)
        target_link_options(${target} PRIVATE "LINKER:--wrap=${api}")
    endforeach()
endfunction()
if(NOT XIAOTAI_HOST_PYTHON)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(XIAOTAI_HOST_PYTHON "${Python3_EXECUTABLE}")
endif()
function(at_builtin_help target)
    add_test(NAME ${target}_help COMMAND "${XIAOTAI_HOST_PYTHON}" -B
             "${CMAKE_CURRENT_SOURCE_DIR}/at_test_help.py" --exact
             --manifest "${CMAKE_CURRENT_SOURCE_DIR}/../engine.json" ${ARGN}
             -- $<TARGET_FILE:${target}> --help-records)
    set_tests_properties(${target}_help PROPERTIES LABELS xiaotai_at)
endfunction()
xiaotai_host_test(at_builtin SOURCES at_builtin_test.c at_test_help.c LINK xiaotai_at)
at_builtin_link_options(at_builtin)
at_builtin_help(at_builtin --with-probes)
add_test(NAME at_builtin_legacy_policy COMMAND at_builtin --legacy-policy)
set_tests_properties(at_builtin_legacy_policy PROPERTIES LABELS xiaotai_at)
add_test(NAME at_builtin_help_no_probe COMMAND "${XIAOTAI_HOST_PYTHON}" -B
         "${CMAKE_CURRENT_SOURCE_DIR}/at_test_help.py" --exact
         --manifest "${CMAKE_CURRENT_SOURCE_DIR}/../engine.json"
         -- $<TARGET_FILE:at_builtin> --help-records --no-probe)
set_tests_properties(at_builtin_help_no_probe PROPERTIES LABELS xiaotai_at)

# Build the actual production sources independently so the off-case cannot
# silently call a guarded handler from the default xiaotai_at archive.
set(at_builtin_sources ${sources} ${guards})
list(TRANSFORM at_builtin_sources PREPEND "../")
foreach(option IN ITEMS RESTART_COMMAND RESTART_FORCE SETTING_WRITE INTENT_COMMAND)
    string(TOLOWER "at_builtin_off_${option}" target)
    xiaotai_host_test(${target} SOURCES at_builtin_test.c at_test_help.c ${at_builtin_sources}
                      INCLUDE_ONLY xiaotai_at LINK xiaotai_framework xiaotai_eventlog xiaotai_port)
    target_compile_definitions(${target} PRIVATE CONFIG_XIAOTAI_AT_${option}=0 XIAOTAI_AT_HOST_PARTIAL=1)
    at_builtin_link_options(${target})
    at_builtin_help(${target} --with-probes --option-off XIAOTAI_AT_${option})
endforeach()
