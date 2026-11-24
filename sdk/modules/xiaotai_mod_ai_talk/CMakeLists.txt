# SPDX-License-Identifier: MIT
# XiaoTai AI talk module (xiaotai/modules/ai_talk.h; manifest name "ai_talk"):
# one spoken conversation with the AI service at a time. Token, link, session
# handshake, audio session, caption, phases, tool dispatch and the orderly end.
# The conversation core, caption ordering and tool/console hooks are portable.
#   src/ai_talk.c            state machine, activity, deadlines, lifecycle
#   src/ai_talk_protocol.c   JSON-RPC requests, replies and notifications
#   src/ai_talk_caption.c    caption merge, ordering filter, UTF-8 cut
#   src/ai_talk_tools.c      tool registry, replies
#   src/ai_talk_args.c       lenient reads of a tool call's arguments
#   src/ai_talk_at.c         AT+AI, diagnostics provider
# Commands and replies are read and written with xiaotai/json.h, not with a
# tokenizer of the module's own.
set(requires xiaotai_port xiaotai_json xiaotai_framework xiaotai_mod_binding xiaotai_platform
             xiaotai_transport xiaotai_media xiaotai_identity xiaotai_eventlog xiaotai_at)
set(sources src/ai_talk.c src/ai_talk_protocol.c src/ai_talk_caption.c src/ai_talk_tools.c src/ai_talk_args.c
            src/ai_talk_at.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" REQUIRES ${requires})
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
add_library(xiaotai_mod_ai_talk STATIC ${sources})
target_include_directories(xiaotai_mod_ai_talk PUBLIC include)
target_link_libraries(xiaotai_mod_ai_talk PUBLIC ${requires})
target_compile_features(xiaotai_mod_ai_talk PUBLIC c_std_11)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
