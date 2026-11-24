# SPDX-License-Identifier: MIT
# XiaoTai device call module (xiaotai/modules/call.h; manifest name "call"):
# voice calls between two devices of one account. Call service requests,
# peer-to-peer link in both roles, the "answered" handshake, audio session,
# endings, and the background release of the call's room on the server.
# Portable sources:
#   src/call.c           state machine, activity, deadlines, lifecycle
#   src/call_signal.c    platform signals: announce, answer, cancel, reject
#   src/call_http.c      requests and their replies, stale-room repair
#   src/call_release.c   release registry of finished calls
#   src/call_signaling.c optional signaling hold and RTC retirement wait
#   src/call_profile.c   device profile scene "call"
#   src/call_at.c        AT+CALL verbs, diagnostics provider
# Signals and replies are read and written with xiaotai/json.h.
# Optional dependency "settings": the microphone setting's name (the call's
# mute returns to that setting). Required only when the product selects it
# (build property XIAOTAI_SELECTED); the component-only host build takes it.
set(requires xiaotai_port xiaotai_json xiaotai_framework xiaotai_mod_contacts xiaotai_platform
             xiaotai_transport xiaotai_media xiaotai_identity xiaotai_eventlog xiaotai_at)
set(sources src/call.c src/call_signal.c src/call_http.c src/call_release.c src/call_profile.c src/call_at.c
    src/call_signaling.c)
if(ESP_PLATFORM)
    idf_build_get_property(selected XIAOTAI_SELECTED)
elseif(XIAOTAI_HOST_PRODUCT_SELECTION)
    set(selected ${XIAOTAI_SELECTED})
else()
    set(selected settings)
endif()
if("settings" IN_LIST selected)
    list(APPEND requires xiaotai_mod_settings)
endif()
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" REQUIRES ${requires})
    if("settings" IN_LIST selected)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE XIAOTAI_CALL_HAS_SETTINGS=1)
    endif()
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
add_library(xiaotai_mod_call STATIC ${sources})
target_include_directories(xiaotai_mod_call PUBLIC include)
target_link_libraries(xiaotai_mod_call PUBLIC ${requires})
if("settings" IN_LIST selected)
    target_compile_definitions(xiaotai_mod_call PRIVATE XIAOTAI_CALL_HAS_SETTINGS=1)
endif()
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
