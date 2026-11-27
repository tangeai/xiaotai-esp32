# SPDX-License-Identifier: MIT
# XiaoTai provisioning module: the application's view of how the device gets
# its network credentials. A thin layer over the network engine: one snapshot
# for setup pages, module events, and the operations a user can ask for
# (start over, give up, try again) with the device wound down first.
# Contract: include/xiaotai/modules/provisioning.h; build options in
# include/xiaotai/modules/provisioning_config.h with the Kconfig next to this
# file.
#
# Portable sources (both branches share the list; no platform sources):
#   src/provisioning.c         status, phase, events, intents, cancel, retry,
#                              lifecycle
#   src/provisioning_begin.c   the begin sequence (quiesce, gate, settings wait,
#                              reprovision, restart hand-over) and the restart
#                              hold after stored credentials
#   src/provisioning_at.c      AT+NET PROVISION [FORCE] / CANCEL, diagnostics
#                              provider
#   src/provisioning_at_forget.c
#                              the FORGET word of AT+NET PROVISION, compiled
#                              only with CONFIG_XIAOTAI_NET_CONSOLE_FORGET (the
#                              network engine's option; the host build compiles
#                              it always, its tests turn the option on); every
#                              function in it is named
#                              xiaotai_provisioning_at_forget_* (manifest
#                              "commands": symbol, checked after linking)
# Optional part, compiled by the selected manifest names (build property
# XIAOTAI_SELECTED; the host build takes every component that exists):
#   src/with_platform.c        "platform" (adds xiaotai_platform): close and
#                              drain the platform gate before a provisioning
#                              restart
#   src/platform_none.c        without "platform": nothing to drain
# The public header includes no optional component.
set(sources src/provisioning.c src/provisioning_begin.c src/provisioning_at.c)
set(requires xiaotai_port xiaotai_framework xiaotai_hal xiaotai_net xiaotai_identity xiaotai_eventlog xiaotai_at)
set(private_requires "")
if(ESP_PLATFORM)
    idf_build_get_property(selected XIAOTAI_SELECTED)
elseif(XIAOTAI_HOST_PRODUCT_SELECTION)
    set(selected ${XIAOTAI_SELECTED})
else()
    set(selected platform)
endif()
if("platform" IN_LIST selected)
    list(APPEND sources src/with_platform.c)
    list(APPEND private_requires xiaotai_platform)
else()
    list(APPEND sources src/platform_none.c)
endif()
if(ESP_PLATFORM)
    if(CONFIG_XIAOTAI_NET_CONSOLE_FORGET)
        list(APPEND sources src/provisioning_at_forget.c)
    endif()
    idf_component_register(SRCS ${sources}
                           INCLUDE_DIRS "include"
                           PRIV_INCLUDE_DIRS "src"
                           REQUIRES ${requires}
                           PRIV_REQUIRES ${private_requires})
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
list(APPEND sources src/provisioning_at_forget.c)
xiaotai_host_require(${requires} ${private_requires})
add_library(xiaotai_mod_provisioning STATIC ${sources})
target_include_directories(xiaotai_mod_provisioning PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_mod_provisioning PUBLIC ${requires} PRIVATE ${private_requires})
target_compile_features(xiaotai_mod_provisioning PUBLIC c_std_11)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
