# SPDX-License-Identifier: MIT
# XiaoTai AI vision module (xiaotai/modules/ai_vision.h; manifest name
# "ai_vision"): still pictures from the camera on the conversation's link
# while an AI conversation runs. Rides on the AI talk module's activity; the
# video engine encodes and paces.
# Portable source only (no platform directory):
#   src/ai_vision.c  attachment and its link video part, session open and close,
#                    switch (setting when configured), diagnostics
# The requirements follow the manifest's depends: of the AI talk module only
# its activity and scene names are used.
set(sources src/ai_vision.c)
set(requires xiaotai_port xiaotai_framework xiaotai_mod_ai_talk xiaotai_video xiaotai_transport
             xiaotai_eventlog xiaotai_at)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" REQUIRES ${requires})
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
add_library(xiaotai_mod_ai_vision STATIC ${sources})
target_include_directories(xiaotai_mod_ai_vision PUBLIC include)
target_link_libraries(xiaotai_mod_ai_vision PUBLIC ${requires})
target_compile_features(xiaotai_mod_ai_vision PUBLIC c_std_11)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
