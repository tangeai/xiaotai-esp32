# SPDX-License-Identifier: MIT
# XiaoTai application "alarm" (xiaotai/apps/alarm.h; manifest name "alarm"):
# an alarm clock on a display. Clock screen, list of alarms, the form that sets
# one, the ringing screen. The alarms are the clock module's schedules; this
# application rings, shows and stops them. Generic views only (CLOCK, LIST,
# FORM), so every theme draws it.
# Portable sources (no platform directory):
#   src/alarm.c   lifecycle, configuration, claim, pages, intents, ringing, AT+ALARM
set(requires xiaotai_port xiaotai_framework xiaotai_mod_clock xiaotai_ui xiaotai_feedback xiaotai_sounds
             xiaotai_eventlog xiaotai_at)
set(sources src/alarm.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" REQUIRES ${requires})
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
add_library(xiaotai_app_alarm STATIC ${sources})
target_include_directories(xiaotai_app_alarm PUBLIC include)
target_link_libraries(xiaotai_app_alarm PUBLIC ${requires})
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
