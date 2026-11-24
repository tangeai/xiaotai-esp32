# SPDX-License-Identifier: MIT
# XiaoTai application "companion" (xiaotai/apps/companion.h; manifest name
# "companion"): the companion intercom. Page flow, what inputs mean, setup
# guidance, rings and spoken hints; one application for touch displays, small
# key-operated displays and boards without a display.
# Portable sources and remaining work packages (no platform directory):
#   src/companion.c            lifecycle, configuration, style choice, foreground, status
#   src/companion_pages.c      page stack, show/back/home, pending requests, toasts
#   src/companion_session.c    companion.session: module calls with the session a view showed
#   src/companion_setup.c      setup guidance: preparing, network, binding, maintenance
#   src/companion_home.c       home and menu pages, idle expression, screen sleep
#   src/companion_settings.c   settings page, the application's own settings
#   src/companion_feedback.c   states and effects, guidance group, built-in tables
#   src/companion_styles.c     built-in styles "pointer", "keys", "plain"
#   src/companion_at.c         verb DISMISS of AT+CALL, diagnostics provider
# One source per optional module, compiled only when the product selects it:
#   src/with_ui.c  with_contacts.c  with_ai_talk.c  with_wake.c  with_call.c
#   with_wechat.c  with_video_call.c  with_room.c  with_stream.c
#   with_stream_video.c  with_recognition.c  with_environment.c
# With the ai_talk module also src/ai_device_tools.c (the conversation's device
# tools: volume, brightness, status) and, with contacts, src/ai_contact_tools.c.
# Each defines one const descriptor xiaotai_app_companion_with_<module>; with
# the sources, this file will generate the NULL-terminated table of the
# descriptors compiled in, so the core never names an optional module.
#
# Components: the manifest's required units (application.json "modules") are
# always required; an optional module's component is required only when the
# product selected it (build property XIAOTAI_SELECTED_COMPONENTS, written by
# scripts/sdk_selection.py). Under ESP-IDF a component outside REQUIRES gives
# no include path, so the two lists below must match the manifest.
set(requires xiaotai_port xiaotai_framework xiaotai_hal xiaotai_mod_provisioning xiaotai_mod_binding
             xiaotai_mod_settings xiaotai_platform xiaotai_media xiaotai_feedback xiaotai_interaction xiaotai_sounds
             xiaotai_eventlog xiaotai_at)
set(optional xiaotai_ui xiaotai_mod_contacts xiaotai_mod_ai_talk xiaotai_mod_wake xiaotai_mod_ai_vision
             xiaotai_mod_call xiaotai_mod_wechat xiaotai_mod_video_call xiaotai_mod_room xiaotai_mod_stream
             xiaotai_mod_stream_video xiaotai_mod_recognition xiaotai_mod_environment xiaotai_transport)
set(core_sources src/companion.c src/companion_pages.c src/companion_styles.c src/companion_setup.c
                 src/companion_home.c src/companion_settings.c src/companion_feedback.c src/companion_aux.c
                 src/companion_audio.c src/companion_session.c src/companion_at.c)
include("${CMAKE_CURRENT_LIST_DIR}/companion_parts.cmake")
if(ESP_PLATFORM)
    idf_build_get_property(selected XIAOTAI_SELECTED_COMPONENTS)
elseif(XIAOTAI_HOST_PRODUCT_SELECTION)
    set(selected ${XIAOTAI_SELECTED_COMPONENTS})
elseif(DEFINED XIAOTAI_APP_COMPANION_WITH)
    set(selected ${XIAOTAI_APP_COMPANION_WITH})
    foreach(component IN LISTS selected)
        if(NOT component IN_LIST optional)
            message(FATAL_ERROR "Unknown companion optional component '${component}'")
        endif()
    endforeach()
else()
    set(selected ${optional})
endif()
foreach(component IN LISTS optional)
    if(component IN_LIST selected)
        list(APPEND requires ${component})
    endif()
endforeach()
if(xiaotai_mod_ai_talk IN_LIST selected)
    list(APPEND parts_shared src/ai_device_tools.c)
    list(APPEND requires xiaotai_json)
endif()
if(xiaotai_mod_contacts IN_LIST selected)
    list(APPEND parts_shared src/companion_contact_route.c)
    if(xiaotai_mod_ai_talk IN_LIST selected)
        list(APPEND parts_shared src/ai_contact_tools.c)
        set(ai_contact_tools ON)
    endif()
endif()
if(xiaotai_mod_call IN_LIST selected OR xiaotai_mod_wechat IN_LIST selected OR xiaotai_mod_video_call IN_LIST selected)
    list(APPEND parts_shared src/companion_call_ui.c)
endif()
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(ESP_PLATFORM OR XIAOTAI_HOST_PRODUCT_SELECTION)
        set(wechat_code_builtin "${CONFIG_XIAOTAI_COMPANION_WECHAT_CODE}")
    else()
        option(XIAOTAI_COMPANION_WECHAT_CODE_BUILTIN "Include the shipped companion QR image" OFF)
        set(wechat_code_builtin "${XIAOTAI_COMPANION_WECHAT_CODE_BUILTIN}")
    endif()
    include("${CMAKE_CURRENT_LIST_DIR}/companion_wechat_code.cmake")
    xiaotai_companion_wechat_code(wechat_code "${selected}" "${CMAKE_CURRENT_LIST_DIR}/../..")
    if(wechat_code_builtin AND NOT wechat_code)
        if(NOT xiaotai_ui IN_LIST selected)
            message(FATAL_ERROR "The shipped companion WeChat code requires xiaotai_ui")
        endif()
        set(wechat_code "${CMAKE_CURRENT_LIST_DIR}/assets/wechat_code.c")
    endif()
    xiaotai_companion_parts(parts "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_BINARY_DIR}/companion_parts.c"
                            "${optional}" "${selected}")
endif()
if(ESP_PLATFORM)
    idf_component_register(SRCS ${core_sources} ${parts} ${parts_shared} ${wechat_code}
                           INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src" REQUIRES ${requires})
    if(xiaotai_transport IN_LIST selected)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE COMPANION_WITH_TRANSPORT=1)
    endif()
    if(ai_contact_tools)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE COMPANION_WITH_AI_CONTACT_TOOLS=1)
    endif()
    if(wechat_code)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE XIAOTAI_COMPANION_WECHAT_CODE_ASSET=1)
    endif()
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake): every
# optional module that exists, unless XIAOTAI_APP_COMPANION_WITH names a subset
# (component names), which host tests use to prove that a product without a
# module still builds and runs.
xiaotai_host_require(${requires})
add_library(xiaotai_app_companion STATIC ${core_sources} ${parts} ${parts_shared} ${wechat_code})
target_include_directories(xiaotai_app_companion PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_app_companion PUBLIC ${requires})
if(xiaotai_transport IN_LIST selected)
    target_compile_definitions(xiaotai_app_companion PRIVATE COMPANION_WITH_TRANSPORT=1)
endif()
if(ai_contact_tools)
    target_compile_definitions(xiaotai_app_companion PRIVATE COMPANION_WITH_AI_CONTACT_TOOLS=1)
endif()
if(wechat_code)
    target_compile_definitions(xiaotai_app_companion PRIVATE XIAOTAI_COMPANION_WECHAT_CODE_ASSET=1)
endif()
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
