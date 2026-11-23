# SPDX-License-Identifier: MIT
# XiaoTai interaction engine: polls the board's input parts, recognizes
# gestures with the timing of an application's interaction profile and posts
# the intents the profile names (xiaotai/interaction.h). Portable: only the
# port layer, the framework, the part interfaces and the console vocabulary.
set(sources src/interaction_core.c src/interaction_recognizer.c src/interaction_task.c src/interaction_commands.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src"
                           REQUIRES xiaotai_port xiaotai_framework xiaotai_hal xiaotai_eventlog xiaotai_at)
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port xiaotai_framework xiaotai_hal xiaotai_eventlog xiaotai_at)
add_library(xiaotai_interaction STATIC ${sources})
target_include_directories(xiaotai_interaction PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_interaction PUBLIC xiaotai_port xiaotai_framework xiaotai_hal xiaotai_eventlog xiaotai_at)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
