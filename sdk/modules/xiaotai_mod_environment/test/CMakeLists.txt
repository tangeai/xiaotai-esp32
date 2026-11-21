# SPDX-License-Identifier: MIT
# Host tests of the environment module (work package ME-1).
#
# mod_environment_<group> builds the module's sources with the real framework,
# event log and host port, and fakes of the platform core, the network
# engine's maintenance gate and the preferences service (fake_platform.c,
# fake_net.c): those engines' headers only (INCLUDE_ONLY), never their
# libraries. One executable per group of environment_test.c. The linker wraps
# the quiesce and restart calls, the key-value flush, the port allocators and
# task creation (see the test's header comment).
# mod_environment_settings_held is the settings group with
# XIAOTAI_ENVIRONMENT_HOLD_ON_SETTINGS_ERROR on.
# mod_environment_optional checks a product without the module (no link to it
# at all: binding's and recognition's headers only).
set(module_sources ../src/environment.c ../src/environment_barrier.c)
set(fakes fake_platform.c fake_net.c)
set(wraps xiaotai_quiesce_begin xiaotai_quiesce_end xiaotai_restart_request xiaotai_kv_flush xiaotai_mem_alloc
          xiaotai_mem_calloc xiaotai_task_create)

foreach(group flow refuse cancel timeout timer_capacity store_timeout settings_frozen settings_held boot_error restart)
    set(name mod_environment_${group})
    xiaotai_host_test(mod_environment_${group}
                      SOURCES environment_test.c fake_environment_at.c ${fakes} ${module_sources}
                      LINK xiaotai_framework xiaotai_eventlog xiaotai_port
                      INCLUDE_ONLY xiaotai_mod_environment xiaotai_net xiaotai_platform xiaotai_identity
                      INCLUDE ../src)
    target_compile_definitions(${name} PRIVATE ENV_TEST_GROUP="${group}")
    if(group STREQUAL "settings_held")
        target_compile_definitions(${name} PRIVATE CONFIG_XIAOTAI_ENVIRONMENT_HOLD_ON_SETTINGS_ERROR=1)
    endif()
    foreach(symbol IN LISTS wraps)
        target_link_options(${name} PRIVATE "LINKER:--wrap=${symbol}")
    endforeach()
endforeach()

get_filename_component(modules_dir "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
xiaotai_host_test(mod_environment_optional
                  SOURCES environment_optional_test.c
                  LINK xiaotai_framework xiaotai_port
                  INCLUDE_ONLY xiaotai_mod_binding xiaotai_mod_recognition)
target_compile_definitions(mod_environment_optional PRIVATE XT_MODULES_DIR="${modules_dir}")

xiaotai_host_test(mod_environment_at
                  SOURCES environment_at_test.c ${fakes} ${module_sources} ../src/environment_at.c
                  LINK xiaotai_framework xiaotai_eventlog xiaotai_port xiaotai_at xiaotai_json
                  INCLUDE_ONLY xiaotai_mod_environment xiaotai_net xiaotai_platform xiaotai_identity
                  INCLUDE ../src ../../../xiaotai_at/src)
target_compile_definitions(mod_environment_at PRIVATE XT_ENV_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/../module.json")
target_link_options(mod_environment_at PRIVATE "LINKER:--wrap=xiaotai_at_notifyf")
add_test(NAME mod_environment_at_busy_base COMMAND mod_environment_at busy-base-first)
set_tests_properties(mod_environment_at_busy_base PROPERTIES TIMEOUT 30 LABELS xiaotai_mod_environment)
