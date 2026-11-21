# SPDX-License-Identifier: MIT
# XiaoTai video call module (xiaotai/modules/video_call.h; manifest name
# "video_call"): video on calls. A session extension of the activity "call":
# contributes the link video part and the profile scene part for the scene of
# each call module the product selects, and runs the video session on an
# active call's link.
# Portable sources (no platform directory):
#   src/video_call.c           scene table, contributions, session, retries, camera switch
#   src/video_call_profile.c   video members of the hosts' profile scenes
#   src/video_call_at.c        AT+CALL VIDEO / CAMERA, diagnostics provider
# One source per optional dependency, compiled only when the product selects
# it (the build passes the selected manifest names in the build property
# XIAOTAI_SELECTED; the host build takes every one that exists). Each defines
# one descriptor in a generated table, so the core never names a call module:
#   src/with_call.c     scene "call": peer is a device; match by xiaotai_call_status
#   src/with_wechat.c   scene "voip": peer is a phone; match by xiaotai_wechat_status,
#                       xiaotai_wechat_request_video (0x1105) once subscribed
#   src/with_ui.c       the UI engine's built-in source (xiaotai_ui_video_show / _clear)
# The manifest's "one_of" makes sure at least one call module is built.
# xiaotai_platform: the scene parts it contributes are xiaotai_platform_scene_part_t
# (XIAOTAI_PLATFORM_SLOT_SCENE); never reached through a call module's requires.
set(requires xiaotai_port xiaotai_framework xiaotai_hal xiaotai_platform xiaotai_video
             xiaotai_transport xiaotai_eventlog xiaotai_at xiaotai_json)
if(ESP_PLATFORM)
    idf_build_get_property(selected XIAOTAI_SELECTED)
elseif(XIAOTAI_HOST_PRODUCT_SELECTION)
    set(selected ${XIAOTAI_SELECTED})
else()
    if(DEFINED XIAOTAI_VIDEO_CALL_HOST_SELECTED)
        set(selected ${XIAOTAI_VIDEO_CALL_HOST_SELECTED})
    else()
        set(selected call wechat ui)
    endif()
endif()
set(sources src/video_call.c src/video_call_profile.c src/video_call_at.c)
set(scene_declarations "")
set(scene_entries "")
set(ui_declaration "")
set(ui_entry "NULL")
foreach(name IN ITEMS call wechat ui)
    if(NOT name IN_LIST selected)
        continue()
    endif()
    if(name STREQUAL "ui")
        list(APPEND requires xiaotai_ui)
        set(ui_declaration "extern const video_call_ui_t video_call_with_ui;")
        set(ui_entry "&video_call_with_ui")
    else()
        list(APPEND requires xiaotai_mod_${name})
        string(APPEND scene_declarations "extern const video_call_host_t video_call_with_${name};\n")
        string(APPEND scene_entries "    &video_call_with_${name},\n")
    endif()
    list(APPEND sources src/with_${name}.c)
endforeach()
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(scene_table "${CMAKE_CURRENT_BINARY_DIR}/video_call_hosts.c")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/src/hosts.c.in" "${scene_table}" @ONLY)
    list(APPEND sources "${scene_table}")
endif()
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS src
                           REQUIRES ${requires})
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
add_library(xiaotai_mod_video_call STATIC ${sources})
target_include_directories(xiaotai_mod_video_call PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_mod_video_call PUBLIC ${requires})
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
