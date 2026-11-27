# SPDX-License-Identifier: MIT
# XiaoTai application "alarm" (xiaotai/apps/alarm.h; manifest name "alarm"):
# an alarm clock on a display. Clock screen, list of alarms, the form that sets
# one, the ringing screen. The alarms are the clock module's schedules; this
# application rings, shows and stops them. Generic views only (CLOCK, LIST,
# FORM), so every theme draws it.
# Portable sources (no platform directory):
#   src/alarm.c            lifecycle, configuration, claim, pages, intents, ringing, AT+ALARM
#   src/alarm_sounds.c     the sounds of an alarm and of a countdown (table: scripts/generate_alarm_sounds.py)
#   src/alarm_ai_tools.c   with the ai_talk module: alarms and countdowns set by talking
# The optional module's component is required only when the product selected
# it (build property XIAOTAI_SELECTED_COMPONENTS, written by
# scripts/sdk_selection.py); a host build without a product takes it.
set(requires xiaotai_port xiaotai_framework xiaotai_mod_clock xiaotai_ui xiaotai_feedback xiaotai_sounds
             xiaotai_eventlog xiaotai_at)
set(optional xiaotai_mod_ai_talk)
set(sources src/alarm.c src/alarm_sounds.c)
if(ESP_PLATFORM)
    idf_build_get_property(selected XIAOTAI_SELECTED_COMPONENTS)
elseif(XIAOTAI_HOST_PRODUCT_SELECTION)
    set(selected ${XIAOTAI_SELECTED_COMPONENTS})
else()
    set(selected ${optional})
endif()
foreach(component IN LISTS optional)
    if(component IN_LIST selected)
        list(APPEND requires ${component})
    endif()
endforeach()
if(xiaotai_mod_ai_talk IN_LIST selected)
    list(APPEND requires xiaotai_json)
    list(APPEND sources src/alarm_ai_tools.c)
    set(ai_tools ON)
endif()
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src" REQUIRES ${requires})
    if(ai_tools)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ALARM_WITH_AI_TOOLS=1)
    endif()
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
add_library(xiaotai_app_alarm STATIC ${sources})
target_include_directories(xiaotai_app_alarm PUBLIC include)
target_link_libraries(xiaotai_app_alarm PUBLIC ${requires})
if(ai_tools)
    target_compile_definitions(xiaotai_app_alarm PRIVATE ALARM_WITH_AI_TOOLS=1)
endif()
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
