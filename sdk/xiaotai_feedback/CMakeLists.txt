# SPDX-License-Identifier: MIT
# XiaoTai feedback engine: shows state on lights and a vibrator and plays
# prompts and rings, driven by the tables applications register
# (xiaotai/feedback.h). Portable: it reaches lights through the output parts
# and the speaker through a player interface.
#
# Optional part, compiled when the product selects the manifest name (build
# property XIAOTAI_SELECTED; host integration may explicitly select media):
#   src/feedback_player_media.c   "media" (adds xiaotai_media to REQUIRES): the
#                                 default player, the media engine's sound
#                                 playback, and the media.output_free binding
#   src/player_none.c             without "media": no default player; rows
#                                 with sounds drive only lights and vibrators
# The public header includes no optional component.
set(common_sources src/feedback_core.c src/feedback_light.c src/feedback_output.c
                   src/feedback_sound.c src/feedback_commands.c)
set(sources ${common_sources})
set(requires xiaotai_port xiaotai_framework xiaotai_hal xiaotai_eventlog xiaotai_at)
if(ESP_PLATFORM)
    idf_build_get_property(selected XIAOTAI_SELECTED)
elseif(XIAOTAI_HOST_PRODUCT_SELECTION)
    set(selected ${XIAOTAI_SELECTED})
elseif(XIAOTAI_FEEDBACK_HOST_MEDIA)
    set(selected media)
else()
    # A host build reaches this file from the component that requires it, with that component's
    # variables in scope: its selection is not this one's.
    set(selected "")
endif()
if("media" IN_LIST selected)
    list(APPEND requires xiaotai_media)
    list(APPEND sources src/feedback_player_media.c)
else()
    list(APPEND sources src/player_none.c)
endif()
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" REQUIRES ${requires})
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
add_library(xiaotai_feedback STATIC ${sources})
target_include_directories(xiaotai_feedback PUBLIC include)
target_link_libraries(xiaotai_feedback PUBLIC ${requires})
if(XIAOTAI_HOST_TESTS)
    xiaotai_host_require(xiaotai_media)
    add_subdirectory(test)
endif()
