# SPDX-License-Identifier: MIT
# XiaoTai clock module: the time of day for applications (gate, local time, a
# minute event, the time zone setting) and schedules (alarms kept across
# restarts, countdowns). It builds on the clock port, the storage port and the
# framework's timers; it shows and rings nothing.
# Portable sources (both branches share the list; no platform sources):
#   src/clock.c            wall clock, gate watching, minute event, lifecycle
#   src/clock_schedule.c   entries, next-due computation, firing
#   src/clock_store.c      the stored alarm table
#   src/clock_at.c         AT+CLOCK, diagnostics provider
set(sources src/clock.c src/clock_schedule.c src/clock_store.c src/clock_at.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include"
                           REQUIRES xiaotai_port xiaotai_framework xiaotai_eventlog xiaotai_at)
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port xiaotai_framework xiaotai_eventlog xiaotai_at)
add_library(xiaotai_mod_clock STATIC ${sources})
target_include_directories(xiaotai_mod_clock PUBLIC include)
target_link_libraries(xiaotai_mod_clock PUBLIC xiaotai_port xiaotai_framework xiaotai_eventlog xiaotai_at)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
