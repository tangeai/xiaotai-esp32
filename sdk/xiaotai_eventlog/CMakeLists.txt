# SPDX-License-Identifier: MIT
# XiaoTai event log: the RAM diagnostics store (recent events with fixed labels,
# and mailboxes for real-time reports). Portable C11 on the port layer (time,
# critical sections, memory by purpose) plus the framework for the engine
# lifecycle; nothing differs between ESP-IDF and the host, so there are no
# esp-idf/ or host/ directories. Build options: include/xiaotai/eventlog_config.h
# with the Kconfig next to this file (the sink and topic tables' capacities).
#
set(sources src/events.c src/topics.c src/reader.c src/engine.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src"
                           REQUIRES xiaotai_port xiaotai_framework)
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port xiaotai_framework)
add_library(xiaotai_eventlog STATIC ${sources})
target_include_directories(xiaotai_eventlog PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_eventlog PUBLIC xiaotai_port xiaotai_framework)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
