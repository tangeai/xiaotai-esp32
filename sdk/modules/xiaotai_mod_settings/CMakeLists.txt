# SPDX-License-Identifier: MIT
# XiaoTai settings module: registers the settings every product shares
# (speaker volume and mute, microphone mute and gain, voice guidance, screen
# sleep time) with the framework's registry and applies the audio ones to the
# media engine. Storage places of deployed devices are a product's preference
# binding table; this module only names the storage keys. Portable: no
# esp-idf/ or host/ directory. Build options: Kconfig and
# include/xiaotai/modules/settings_config.h.
#
# Portable sources (both branches share the list):
#   src/settings.c         configuration, screen.sleep, status, events, lifecycle
#   src/settings_diag.c    diagnostics provider "settings"
#   src/selection.c        explicit confirmation by a view, independent of AT+SET
# Optional part, chosen by whether the product selects the media engine (build
# property XIAOTAI_SELECTED_COMPONENTS, written by scripts/sdk_selection.py;
# the host build takes every component that exists, so it has the engine):
#   src/with_media.c       with xiaotai_media (added to REQUIRES): the audio
#                          settings, applying them to the media engine, taking
#                          back, preview, steps, intents
#   src/audio_none.c       without it: no audio setting is registered and the
#                          audio functions answer XIAOTAI_ERR_NOT_SUPPORTED
# The public header includes no optional component.
set(sources src/settings.c src/settings_diag.c src/selection.c)
set(framework_internal "../../xiaotai_framework/internal")
set(requires xiaotai_port xiaotai_framework xiaotai_hal xiaotai_identity xiaotai_eventlog xiaotai_at)
if(ESP_PLATFORM)
    idf_build_get_property(selected XIAOTAI_SELECTED_COMPONENTS)
    if(xiaotai_media IN_LIST selected)
        list(APPEND sources src/with_media.c)
        list(APPEND requires xiaotai_media)
    else()
        list(APPEND sources src/audio_none.c)
    endif()
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src" "${framework_internal}"
                           REQUIRES ${requires})
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake). The library
# is the build with the media engine; the test mod_settings_no_media compiles
# audio_none.c into its own executable.
if(NOT XIAOTAI_HOST_PRODUCT_SELECTION OR xiaotai_media IN_LIST XIAOTAI_SELECTED_COMPONENTS)
    list(APPEND sources src/with_media.c)
    list(APPEND requires xiaotai_media)
else()
    list(APPEND sources src/audio_none.c)
endif()
xiaotai_host_require(${requires})
add_library(xiaotai_mod_settings STATIC ${sources})
target_include_directories(xiaotai_mod_settings PUBLIC include PRIVATE src "${framework_internal}")
target_link_libraries(xiaotai_mod_settings PUBLIC ${requires})
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
