# SPDX-License-Identifier: MIT
# XiaoTai room module: multi-party push-to-talk rooms (xiaotai/modules/room.h).
# It mirrors the server's assignment, keeps the device connected to its room
# while an application attends it (token, link, join, lease heartbeat, member
# list) and carries push-to-talk. The platform core carries every request and
# signal, the transport the link, the media engine the audio.
#
# Portable sources and remaining work packages (no platform sources):
#   src/room.c              state, status, events, attendance, lifecycle
#   src/room_assignment.c   assignment sync, create, join, leave, validation
#   src/room_connect.c      token, lease, link, join handshake, retry and release
#   src/room_signal.c       room commands: snapshot, members, microphone states
#   src/room_members.c      the member list and its paged copy
#   src/room_talk.c         push-to-talk gate, keep-alive, speaker announcements
#   src/room_at.c           AT+ROOM and diagnostics provider
# Replies and room commands are read with xiaotai/json.h (count first, then a
# token table from BULK for the time of the handling): no tokenizer of the
# module's own. Room commands wait in the transport engine's command pool
# under the share the module declares in init (xiaotai_transport_require).
set(requires xiaotai_port xiaotai_json xiaotai_framework xiaotai_mod_binding xiaotai_platform
             xiaotai_identity xiaotai_transport xiaotai_media xiaotai_eventlog xiaotai_at)
set(sources src/room.c src/room_assignment.c src/room_connect.c src/room_signal.c src/room_members.c src/room_talk.c
            src/room_at.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" REQUIRES ${requires})
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
add_library(xiaotai_mod_room STATIC ${sources})
target_include_directories(xiaotai_mod_room PUBLIC include)
target_link_libraries(xiaotai_mod_room PUBLIC ${requires})
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
