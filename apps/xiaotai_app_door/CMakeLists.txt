# SPDX-License-Identifier: MIT
# XiaoTai application "door" (xiaotai/apps/door.h; manifest name "door"): the
# indoor station of a video door phone. Door home screen, choice of the door
# station, the door call page, the confirmation before a door is opened. Which
# events it presents is its manifest's "handles" and its claim().
# Portable sources (no platform directory):
#   src/door.c              lifecycle, configuration, foreground, claim, return, status
#   src/door_home.c         door home screen, pager, device list, monitor page
#   src/door_call.c         door call classification and page, door.call, mute, volume, ring states
#   src/door_open.c         confirmation, lock request, result
#   src/door_station.c      door station: directory, calls it dials (device and WeChat), calling one after the other
#   src/door_peers.c        the chosen counterparts (settings)
#   src/door_pages.c        every mode a page of its own (option mode_pages), each with its counterparts
#   src/door_look.c         looking at the door, both ends
#   src/door_mode.c         choice of mode, settings page, questions, recent calls
#   src/door_key.c          the board's main key: bell, answer, hang up
#   src/door_sounds.c       its sounds (table: scripts/generate_door_sounds.py)
#   src/door_access.c       the access machine: keypad, households; src/door_rooms.c: rooms in names
#   src/door_fallback.c     what reaches it because no other application presents it (other
#                           calls, setup guidance, the code announcement) with generic views
#   src/door_at.c           AT+DOOR, diagnostics provider
# Every LVGL display: src/templates_station.c (somebody looks, the door was opened, the keypad).
# Large colour displays (option DOOR_TEMPLATES) add src/templates_wide.c, src/backdrop.c
# (the pictures, drawn into memory) and flash-resident icons and fonts; smaller ones show
# the theme's pages.
# Feature adapters, compiled only when the product selects their modules:
#   src/with_video_call.c  with_stream.c  with_stream_video.c  with_lock.c
# Each defines a const door_with_<module> descriptor for the selected table.
# WeChat contributes only fallback presentation, in src/door_fallback.c.
#
# Components: the manifest's required units always; an optional module's
# component only when the product selected it (build property
# XIAOTAI_SELECTED_COMPONENTS). The two lists below must match the manifest.
set(requires xiaotai_port xiaotai_framework xiaotai_mod_call xiaotai_mod_contacts xiaotai_mod_provisioning
             xiaotai_mod_binding xiaotai_mod_settings xiaotai_net xiaotai_platform xiaotai_ui xiaotai_media xiaotai_feedback xiaotai_interaction xiaotai_sounds
             xiaotai_eventlog xiaotai_at xiaotai_hal)
set(optional xiaotai_mod_video_call xiaotai_mod_stream xiaotai_mod_stream_video xiaotai_mod_lock xiaotai_mod_wechat)
set(sources src/door.c src/door_home.c src/door_call.c src/door_open.c src/door_at.c src/door_fallback.c
            src/door_station.c src/door_peers.c src/door_pages.c src/door_look.c src/door_mode.c src/door_key.c src/door_sounds.c src/door_access.c src/door_rooms.c)
if(ESP_PLATFORM)
    idf_build_get_property(selected XIAOTAI_SELECTED_COMPONENTS)
elseif(XIAOTAI_HOST_PRODUCT_SELECTION)
    set(selected ${XIAOTAI_SELECTED_COMPONENTS})
elseif(DEFINED XIAOTAI_APP_DOOR_WITH)
    set(selected ${XIAOTAI_APP_DOOR_WITH})
else()
    set(selected ${optional})
endif()
foreach(component IN LISTS optional)
    if(component IN_LIST selected)
        string(REPLACE "xiaotai_mod_" "" feature "${component}")
        string(TOUPPER "${feature}" feature_upper)
        list(APPEND requires ${component})
        if(NOT feature STREQUAL "wechat")
            list(APPEND sources "src/with_${feature}.c")
        endif()
        list(APPEND defines "XIAOTAI_DOOR_WITH_${feature_upper}")
    endif()
endforeach()
if(ESP_PLATFORM)
    set(door_wide ${CONFIG_XIAOTAI_DOOR_TEMPLATES})
    set(door_pages ${CONFIG_XIAOTAI_UI_LVGL})
else()
    xiaotai_host_require(${requires})
    if(TARGET lvgl::lvgl)
        set(door_wide ON)
        set(door_pages ON)
    endif()
endif()
if(door_pages)
    list(APPEND sources src/templates_station.c assets/fonts/door_font_digits.c)
    list(APPEND defines XIAOTAI_DOOR_PAGES)
endif()
if(door_wide)
    set(door_wide_sources src/templates_wide.c src/backdrop.c assets/door_v4_assets.c assets/door_station_icons.c)
    foreach(font small button body dialog title clock clock_small)
        list(APPEND door_wide_sources "assets/fonts/door_font_${font}.c")
    endforeach()
    list(APPEND sources ${door_wide_sources})
    list(APPEND defines XIAOTAI_DOOR_WIDE)
endif()
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "assets" REQUIRES ${requires})
    if(door_pages)
        idf_component_optional_requires(PRIVATE lvgl lvgl__lvgl)
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${defines})
    # A whole screen is worked out pixel by pixel while the page waits.
    set_source_files_properties(src/backdrop.c PROPERTIES COMPILE_OPTIONS "-O2")
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake): every
# optional module that exists, unless XIAOTAI_APP_DOOR_WITH names a subset.
add_library(xiaotai_app_door STATIC ${sources})
target_include_directories(xiaotai_app_door PUBLIC include PRIVATE assets)
target_link_libraries(xiaotai_app_door PUBLIC ${requires})
if(door_pages)
    target_link_libraries(xiaotai_app_door PRIVATE lvgl::lvgl)
endif()
target_compile_definitions(xiaotai_app_door PRIVATE ${defines})
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
