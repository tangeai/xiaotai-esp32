# SPDX-License-Identifier: MIT
# XiaoTai contacts module: mirrors the server's contact list for the present
# binding from the source the product names (paged view, lookups for the call
# modules, online queries), and where the source serves them the pending
# friendship requests and the friendship changes between devices.
# Portable only: nothing differs between ESP-IDF and the host, so there are no
# esp-idf/ or host/ directories. Build option in Kconfig, mapped in
# include/xiaotai/modules/contacts_config.h.
#
#   src/contacts.c           cache, view, status, events, intents, lifecycle
#   src/contacts_parse.c     the list reply, read through the source descriptor
#                            with validated JSON views; the token limit stays
#                            in effect without a table or per-entry allocation
#   src/contacts_fetch.c     when to fetch, deferral, stale replies, queries
#   src/contacts_match.c     name normalization and matching
#   src/contacts_change.c    friendship changes and the pending page
#   src/contacts_at.c        local commands and the diagnostics provider
set(sources src/contacts.c src/contacts_parse.c src/contacts_fetch.c src/contacts_match.c
            src/contacts_change.c src/contacts_at.c)
set(requires xiaotai_port xiaotai_json xiaotai_framework xiaotai_mod_binding xiaotai_platform xiaotai_identity
             xiaotai_eventlog xiaotai_at xiaotai_net)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src" REQUIRES ${requires})
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
add_library(xiaotai_mod_contacts STATIC ${sources})
target_include_directories(xiaotai_mod_contacts PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_mod_contacts PUBLIC ${requires})
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
