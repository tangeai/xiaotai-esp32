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
#   src/door_fallback.c     what reaches it because no other application presents it (other
#                           calls, setup guidance, the code announcement) with generic views
#   src/door_at.c           AT+DOOR, diagnostics provider
# Wide colour displays add src/templates_wide.c and flash-resident assets.
# Feature adapters, compiled only when the product selects their modules:
#   src/with_video_call.c  with_stream.c  with_stream_video.c  with_lock.c
# Each defines a const door_with_<module> descriptor for the selected table.
# WeChat contributes only fallback presentation, in src/door_fallback.c.
#
# Components: the manifest's required units always; an optional module's
# component only when the product selected it (build property
# XIAOTAI_SELECTED_COMPONENTS). The two lists below must match the manifest.
set(requires xiaotai_port xiaotai_framework xiaotai_mod_call xiaotai_mod_contacts xiaotai_mod_provisioning
             xiaotai_mod_binding xiaotai_mod_settings xiaotai_net xiaotai_platform xiaotai_ui xiaotai_feedback xiaotai_sounds
             xiaotai_eventlog xiaotai_at)
set(optional xiaotai_mod_video_call xiaotai_mod_stream xiaotai_mod_stream_video xiaotai_mod_lock xiaotai_mod_wechat)
set(sources src/door.c src/door_home.c src/door_call.c src/door_open.c src/door_at.c src/door_fallback.c)
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
    set(door_wide ${CONFIG_XIAOTAI_UI_LVGL})
else()
    xiaotai_host_require(${requires})
    if(TARGET lvgl::lvgl)
        set(door_wide ON)
        enable_language(ASM)
    endif()
endif()
if(door_wide)
    set(door_wallpaper "${CMAKE_CURRENT_LIST_DIR}/assets/door_v4_wallpaper.rgb565")
    set(wallpaper_src "${CMAKE_CURRENT_BINARY_DIR}/door_wallpaper.S")
    configure_file(assets/door_wallpaper.S.in "${wallpaper_src}" @ONLY)
    set_source_files_properties("${wallpaper_src}" PROPERTIES OBJECT_DEPENDS "${door_wallpaper}")
    set(door_wide_sources src/templates_wide.c assets/door_v4_assets.c "${wallpaper_src}")
    foreach(font small button body dialog title clock)
        list(APPEND door_wide_sources "assets/fonts/door_font_${font}.c")
    endforeach()
    list(APPEND sources ${door_wide_sources})
    list(APPEND defines XIAOTAI_DOOR_WIDE)
endif()
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "assets" REQUIRES ${requires})
    if(door_wide)
        idf_component_optional_requires(PRIVATE lvgl lvgl__lvgl)
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${defines})
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake): every
# optional module that exists, unless XIAOTAI_APP_DOOR_WITH names a subset.
add_library(xiaotai_app_door STATIC ${sources})
target_include_directories(xiaotai_app_door PUBLIC include PRIVATE assets)
target_link_libraries(xiaotai_app_door PUBLIC ${requires})
if(door_wide)
    target_link_libraries(xiaotai_app_door PRIVATE lvgl::lvgl)
endif()
target_compile_definitions(xiaotai_app_door PRIVATE ${defines})
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
