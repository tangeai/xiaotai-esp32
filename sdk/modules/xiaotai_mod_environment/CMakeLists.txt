# SPDX-License-Identifier: MIT
# XiaoTai environment module: coordinates a switch of the service environment
# (quiesce, the network engine's maintenance gate, the platform gate, a storage
# barrier, store and read back, restart) and owns the AT+EP command.
# Options: Kconfig, mapped in include/xiaotai/modules/environment_config.h.
#
# Portable sources (both branches share the list; no platform sources: the
# helper task that waits for storage is a port task):
#   src/environment.c         the phase machine, events, intents (vetoes are
#                             the framework's quiesce guards, not a table here)
#                             (work package ME-1)
#   src/environment_barrier.c the storage barriers on a short-lived task (before
#                             the store; on release after a timed-out store)
#                             (ME-1)
#   src/environment_at.c      AT+EP, its unsolicited result line (it
#                             subscribes to environment.result), diagnostics
#                             provider; registered from
#                             xiaotai_module_environment_init
# Internal header: src/environment_internal.h.
set(sources src/environment.c src/environment_barrier.c src/environment_at.c)
set(requires xiaotai_port xiaotai_framework xiaotai_net xiaotai_platform xiaotai_identity xiaotai_eventlog
             xiaotai_at)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src" REQUIRES ${requires})
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
add_library(xiaotai_mod_environment STATIC ${sources})
target_include_directories(xiaotai_mod_environment PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_mod_environment PUBLIC ${requires})
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
