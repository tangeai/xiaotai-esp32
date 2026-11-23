# SPDX-License-Identifier: MIT
# XiaoTai binding module: the flow that gives the device its identity (report,
# verification code, grant over a guest signaling session, store, confirm),
# preparation steps of other modules, the signed re-binding after a
# server-side unbind, speaking the code on request and the local reset. The
# platform core carries every exchange; the identity component stores the
# result. Portable: nothing differs between ESP-IDF and the host, so there are
# no esp-idf/ or host/ directories.
#
# Sources so far (work package MB-1):
#   src/binding.c            phases, status, events, intents, guards, preparation
#                            steps, the framework quiesce guard "binding"
#                            (refuses a switch of the service base during an
#                            attempt), the identity jobs' hand-off, lifecycle
#   src/binding_report.c     the device report and its reply
#   src/binding_grant.c      guest session, grant parsing, store, confirmation
#                            (replies and grants read with the JSON library,
#                            xiaotai/json.h)
#   src/binding_window.c     the waiting window (token expiry, stale codes)
#   src/binding_reset.c      local reset and ordered identity writes (MB-3)
#   src/binding_at.c         AT+BIND, AT+BIND RETRY / REPLAY, diagnostics
#                            provider (MB-3)
#   src/binding_at_dev.c     AT+BIND SET / CLEAR, compiled only with
#                            CONFIG_XIAOTAI_BINDING_DEV_COMMANDS (the host
#                            build compiles it always, its test turns the
#                            option on); every function in it is named
#                            xiaotai_binding_at_dev_* (manifest "commands":
#                            symbol, checked after linking) (MB-3)
# Optional parts (MB-2), compiled by the selected manifest names (build
# property XIAOTAI_SELECTED; the host build takes every component that
# exists), each adding its component to REQUIRES:
#   src/binding_announce.c   "media" (which brings "sounds"): local digits,
#                            server rendering
#   src/announce_none.c      without "media": the announcement functions
#                            answer XIAOTAI_ERR_NOT_SUPPORTED
# Either attaches itself to the core through binding_voice_attach()
# (src/binding_internal.h). The public headers include neither optional
# component, so the lists below name only what the module always needs.
set(sources src/binding.c src/binding_report.c src/binding_grant.c src/binding_window.c
            src/binding_reset.c src/binding_at.c)
set(requires xiaotai_port xiaotai_json xiaotai_framework xiaotai_mod_provisioning xiaotai_net
             xiaotai_platform xiaotai_identity xiaotai_eventlog xiaotai_at)
if(ESP_PLATFORM)
    idf_build_get_property(selected XIAOTAI_SELECTED)
    if(CONFIG_XIAOTAI_BINDING_DEV_COMMANDS)
        list(APPEND sources src/binding_at_dev.c)
    endif()
elseif(XIAOTAI_HOST_PRODUCT_SELECTION)
    set(selected ${XIAOTAI_SELECTED})
    if(CONFIG_XIAOTAI_BINDING_DEV_COMMANDS)
        list(APPEND sources src/binding_at_dev.c)
    endif()
else()
    set(selected media)
    list(APPEND sources src/binding_at_dev.c)
endif()
if("media" IN_LIST selected)
    list(APPEND sources src/binding_announce.c)
    list(APPEND requires xiaotai_media xiaotai_sounds)
else()
    list(APPEND sources src/announce_none.c)
endif()
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src" REQUIRES ${requires})
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
add_library(xiaotai_mod_binding STATIC ${sources})
target_include_directories(xiaotai_mod_binding PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_mod_binding PUBLIC ${requires})
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
