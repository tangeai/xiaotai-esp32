# SPDX-License-Identifier: MIT
# XiaoTai WeChat voice module (xiaotai/modules/wechat.h; manifest name
# "wechat"): voice calls between the device and a WeChat user. Voice service
# request, pairing with the pushed room, WHIP link, the "answered" handshake,
# audio session, endings and refusals. The binding module arrives through the
# contacts module.
# Portable sources (no platform directory):
#   src/wechat.c           state machine, activity, deadlines, lifecycle
#   src/wechat_rooms.c     room ownership, early rooms, closed-call memory
#   src/wechat_signal.c    platform signals and link commands
#   src/wechat_reject.c    refusals through the RTC service request
#   src/wechat_profile.c   device profile scene "voip", audio format choice
#   src/wechat_at.c        AT+CALL verbs, diagnostics provider
# Signals and replies are read and written with xiaotai/json.h.
# Optional dependency "settings": the microphone setting's name (the call's
# mute returns to that setting). Required only when the product selects it
# (build property XIAOTAI_SELECTED); the component-only host build takes it.
set(sources src/wechat.c src/wechat_rooms.c src/wechat_signal.c src/wechat_reject.c src/wechat_profile.c src/wechat_at.c)
set(requires xiaotai_port xiaotai_json xiaotai_framework xiaotai_hal xiaotai_mod_contacts xiaotai_mod_binding
             xiaotai_platform xiaotai_transport xiaotai_media xiaotai_identity xiaotai_eventlog
             xiaotai_at)
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
        target_compile_definitions(${COMPONENT_LIB} PRIVATE XIAOTAI_WECHAT_SETTINGS=1)
    endif()
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
add_library(xiaotai_mod_wechat STATIC ${sources})
target_include_directories(xiaotai_mod_wechat PUBLIC include)
target_link_libraries(xiaotai_mod_wechat PUBLIC ${requires})
if("settings" IN_LIST selected)
    target_compile_definitions(xiaotai_mod_wechat PRIVATE XIAOTAI_WECHAT_SETTINGS=1)
endif()
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
