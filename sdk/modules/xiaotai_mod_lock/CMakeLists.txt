# SPDX-License-Identifier: MIT
# XiaoTai lock module: the door lock outputs of the device. Claims the output
# role family "door" and drives the board's door relays, releases one for a
# bounded time and locks it again, locks everything when the system is
# quiesced, and carries the door state a contact reports. It decides nothing
# about who may open.
# Contract only for now: include/xiaotai/modules/lock.h and lock_config.h, and
# the options in Kconfig. The sources join with the work packages of the
# module's specification.
#
# Planned portable sources (both branches share the list; no platform sources:
# the relay is reached through its part interface):
#   src/lock.c          claim, discovery, release and deadline, rest time,
#                       fault retry, quiesce, door state, status, events,
#                       intents, lifecycle
#   src/lock_at.c       AT+LOCK, diagnostics provider
#   src/lock_at_open.c  AT+LOCK OPEN / CLOSE, compiled only with
#                       CONFIG_XIAOTAI_LOCK_AT_OPEN (the host build compiles it
#                       always, its test turns the option on); every function
#                       in it is named xiaotai_lock_at_open_* (manifest
#                       "commands": symbol, checked after linking)
if(ESP_PLATFORM)
    idf_component_register(INCLUDE_DIRS "include"
                           REQUIRES xiaotai_port xiaotai_framework xiaotai_hal xiaotai_eventlog xiaotai_at)
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port xiaotai_framework xiaotai_hal xiaotai_eventlog xiaotai_at)
add_library(xiaotai_mod_lock INTERFACE)
target_include_directories(xiaotai_mod_lock INTERFACE include)
target_link_libraries(xiaotai_mod_lock INTERFACE xiaotai_port xiaotai_framework xiaotai_hal xiaotai_eventlog
                      xiaotai_at)
