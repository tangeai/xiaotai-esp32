# SPDX-License-Identifier: MIT
# XiaoTai local wake module (xiaotai/modules/wake.h; manifest name "wake"; the
# engine it drives is "wake_engine" in sdk/xiaotai_wake): when the device
# listens for the wake word and what an accepted detection leads to (pre-roll
# held, conversation started, application told).
# Portable sources (no platform directory):
#   src/wake.c       gate, detection handling, switch (setting when configured), intent, lifecycle
#   src/wake_at.c    AT+WAKE TEST and SELFTEST, diagnostics provider "wake"
#                    (writes its own +WAKE-MODULE: prefix; +WAKE: is the engine's)
set(requires xiaotai_port xiaotai_framework xiaotai_mod_ai_talk xiaotai_wake xiaotai_media
             xiaotai_eventlog xiaotai_at)
set(sources src/wake.c src/wake_at.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src" REQUIRES ${requires})
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
add_library(xiaotai_mod_wake STATIC ${sources})
target_include_directories(xiaotai_mod_wake PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_mod_wake PUBLIC ${requires})
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
