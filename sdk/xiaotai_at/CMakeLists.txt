# SPDX-License-Identifier: MIT
# XiaoTai console engine: line input and the one AT vocabulary shared by every
# product. Portable: it reads and writes through xiaotai_at_io_t sources (the
# port's console, or a test's fake), runs commands through the framework, takes
# the board's identity from the hardware abstraction and reads the event log.
# The contract is include/xiaotai/at.h (the console), include/xiaotai/at_command.h
# (what command authors use) and include/xiaotai/at_config.h with the Kconfig
# next to this file (the registration tables' capacities and the options of the
# engine's guarded commands). One portable source list is shared by both
# branches; there is no platform directory. The implementation joins package by
# package: text helpers, reply framing, verbs, console, and diagnostic
# built-ins. The handlers of each guarded command go into a source of
# their own, added to the list only when its option is on
# (XIAOTAI_AT_RESTART_COMMAND, XIAOTAI_AT_RESTART_FORCE,
# XIAOTAI_AT_SETTING_WRITE, XIAOTAI_AT_INTENT_COMMAND; the host build takes the
# header defaults, on), and are named with the prefix engine.json gives as
# "symbol", so a build with an option off is checked after linking to contain
# none of them. The engine's own commands are declared in engine.json
# ("commands").
set(sources src/at_text.c src/at_reply.c src/at_verbs.c src/at_console.c src/at_plain.c
            src/at_stream.c src/at_notify.c src/at_platform_source.c
            src/at_local.c src/at_log.c src/at_framework.c)
set(guards src/at_guarded_restart.c src/at_guarded_force.c src/at_guarded_set.c src/at_guarded_intent.c)
if(ESP_PLATFORM)
    if(CONFIG_XIAOTAI_AT_RESTART_COMMAND)
        list(APPEND sources src/at_guarded_restart.c)
        if(CONFIG_XIAOTAI_AT_RESTART_FORCE)
            list(APPEND sources src/at_guarded_force.c)
        endif()
    endif()
    if(CONFIG_XIAOTAI_AT_SETTING_WRITE)
        list(APPEND sources src/at_guarded_set.c)
    endif()
    if(CONFIG_XIAOTAI_AT_INTENT_COMMAND)
        list(APPEND sources src/at_guarded_intent.c)
    endif()
    idf_component_register(SRCS ${sources}
                           INCLUDE_DIRS "include"
                           PRIV_INCLUDE_DIRS "src"
                           REQUIRES xiaotai_port xiaotai_framework xiaotai_hal xiaotai_eventlog)
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port xiaotai_framework xiaotai_hal xiaotai_eventlog)
add_library(xiaotai_at STATIC ${sources} ${guards})
target_compile_definitions(xiaotai_at PRIVATE XIAOTAI_AT_HOST_PARTIAL=1)
target_include_directories(xiaotai_at PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_at PUBLIC xiaotai_port xiaotai_framework xiaotai_hal xiaotai_eventlog)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
