# SPDX-License-Identifier: MIT
# Host tests of the event log, under the address and undefined-behaviour
# sanitizers. eventlog_config checks the contract (the build options in Kconfig
# and xiaotai/eventlog_config.h agree and reach the public header);
# eventlog_events the ring (EL-1). The topic, reader and engine tests join with
# EL-2 (eventlog_topics, eventlog_reader, eventlog_engine).
xiaotai_host_test(eventlog_config SOURCES eventlog_config_test.c eventlog_config_override.c
                  INCLUDE_ONLY xiaotai_eventlog)
target_compile_definitions(eventlog_config PRIVATE XIAOTAI_TEST_KCONFIG="${CMAKE_CURRENT_SOURCE_DIR}/../Kconfig")

# Includes ../src for the default resolution and to place the sequence just
# below its wrap point.
xiaotai_host_test(eventlog_events SOURCES eventlog_events_test.c LINK xiaotai_eventlog
                  INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_options(eventlog_events PRIVATE "LINKER:--wrap=xiaotai_time_us")
xiaotai_host_test(eventlog_topics SOURCES eventlog_topics_test.c LINK xiaotai_eventlog)
target_link_options(eventlog_topics PRIVATE "LINKER:--wrap=xiaotai_time_us"
                                            "LINKER:--wrap=xiaotai_mem_has_external")
xiaotai_host_test(eventlog_reader SOURCES eventlog_reader_test.c LINK xiaotai_eventlog
                  INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_options(eventlog_reader PRIVATE "LINKER:--wrap=xiaotai_task_create"
                                            "LINKER:--wrap=xiaotai_mem_has_external")
xiaotai_host_test(eventlog_engine SOURCES eventlog_engine_test.c LINK xiaotai_eventlog)
