# SPDX-License-Identifier: MIT
# Only the policy read: no capture, playback, storage or external RAM.
set(sources src/recognition_config.c src/recognition_config_at.c src/recognition_json.c)
set(requires xiaotai_port xiaotai_json xiaotai_framework xiaotai_platform xiaotai_at)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS include REQUIRES ${requires})
    return()
endif()

xiaotai_host_require(${requires})
add_library(xiaotai_mod_recognition_config STATIC ${sources})
target_include_directories(xiaotai_mod_recognition_config PUBLIC include)
target_link_libraries(xiaotai_mod_recognition_config PUBLIC ${requires})
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
