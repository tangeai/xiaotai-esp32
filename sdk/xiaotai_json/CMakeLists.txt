# SPDX-License-Identifier: MIT
# XiaoTai JSON reader and writer (include/xiaotai/json.h): tokenizes into a
# caller's table and writes into a caller's buffer, never allocates. A library
# at the level of the port: no manifest, no lifecycle; every product links it
# (the selection's base components). Portable C only (the port's result codes
# and the C library): nothing differs between ESP-IDF and the host, so there
# are no esp-idf/ or host/ directories.
set(sources src/json_parse.c src/json_query.c src/json_write.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src" REQUIRES xiaotai_port)
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port)
add_library(xiaotai_json STATIC ${sources})
target_include_directories(xiaotai_json PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_json PUBLIC xiaotai_port)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
