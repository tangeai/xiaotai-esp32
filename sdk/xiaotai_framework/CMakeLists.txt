# SPDX-License-Identifier: MIT
# XiaoTai runtime framework: registers and starts engines, base modules and
# applications, routes intents and events, runs owner-task timers, arbitrates
# the foreground activity, decides which application presents an event,
# quiesces the system for maintenance, performs the orderly restart, and keeps
# the settings, command and diagnostics registries. Portable: only the port
# layer (no board is touched, so it runs on the host without one; settings
# reach the preferences service through the store the identity engine
# installs, not through a link to it). Build options: Kconfig and
# include/xiaotai/framework_config.h.
set(sources src/framework.c src/settings.c src/commands.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src" "internal"
                           REQUIRES xiaotai_port)
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port)
add_library(xiaotai_framework STATIC ${sources})
target_include_directories(xiaotai_framework PUBLIC include PRIVATE src internal)
target_link_libraries(xiaotai_framework PUBLIC xiaotai_port)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
