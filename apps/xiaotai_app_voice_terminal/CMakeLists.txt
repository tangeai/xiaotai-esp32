# SPDX-License-Identifier: MIT
# XiaoTai application "voice_terminal" (xiaotai/apps/voice_terminal.h; manifest
# name "voice_terminal"): a voice terminal without a display. One key, lights,
# rings and cues around WeChat voice calls, and two local resets.
# Portable sources (no platform directory):
#   src/terminal.c           lifecycle, configuration, status, diagnostics provider
#   src/terminal_key.c       key rules: main, end, gesture snapshot, refusals
#   src/terminal_state.c     which states are on, ring and ring-back, call cues
#   src/terminal_reset.c     reset, store retry, and restart hold
#   src/terminal_binding.c   spoken binding-code policy
#   src/terminal_tables.c    interaction profile "terminal-key" and the feedback table
# One source per optional module, compiled only when the product selects it:
#   src/with_environment.c  requests cancellation and waits for its result
# The optional settings module supplies guidance.enabled to terminal_binding.c.
#
# Components: the manifest's required units always; an optional module's
# component only when the product selected it (build property
# XIAOTAI_SELECTED_COMPONENTS). The two lists below must match the manifest.
set(requires xiaotai_port xiaotai_framework xiaotai_mod_wechat xiaotai_mod_contacts xiaotai_mod_provisioning
             xiaotai_mod_binding xiaotai_net xiaotai_platform
             xiaotai_feedback xiaotai_interaction xiaotai_sounds xiaotai_eventlog xiaotai_at)
set(sources src/terminal.c src/terminal_key.c src/terminal_state.c src/terminal_tables.c
            src/terminal_reset.c src/terminal_binding.c)
set(optional xiaotai_mod_environment xiaotai_mod_settings)
if(ESP_PLATFORM)
    idf_build_get_property(selected XIAOTAI_SELECTED_COMPONENTS)
elseif(XIAOTAI_HOST_PRODUCT_SELECTION)
    set(selected ${XIAOTAI_SELECTED_COMPONENTS})
elseif(DEFINED XIAOTAI_APP_VOICE_TERMINAL_WITH)
    set(selected ${XIAOTAI_APP_VOICE_TERMINAL_WITH})
else()
    set(selected ${optional})
endif()
foreach(component IN LISTS optional)
    if(component IN_LIST selected)
        list(APPEND requires ${component})
    endif()
endforeach()
if(xiaotai_mod_environment IN_LIST selected)
    list(APPEND sources src/with_environment.c)
    list(APPEND defines XIAOTAI_TERMINAL_WITH_ENVIRONMENT)
endif()
if(xiaotai_mod_settings IN_LIST selected)
    list(APPEND defines XIAOTAI_TERMINAL_WITH_SETTINGS)
endif()
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" REQUIRES ${requires})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${defines})
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake): every
# optional module that exists, unless XIAOTAI_APP_VOICE_TERMINAL_WITH names a subset.
xiaotai_host_require(${requires})
add_library(xiaotai_app_voice_terminal STATIC ${sources})
target_include_directories(xiaotai_app_voice_terminal PUBLIC include)
target_link_libraries(xiaotai_app_voice_terminal PUBLIC ${requires})
target_compile_definitions(xiaotai_app_voice_terminal PRIVATE ${defines})
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
