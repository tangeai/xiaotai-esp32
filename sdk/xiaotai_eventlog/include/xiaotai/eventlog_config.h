// SPDX-License-Identifier: MIT
#pragma once
/* Build-time options of the event log (xiaotai/config.h explains the
 * mechanism; the options themselves are in the component's Kconfig).
 *
 * Only the capacities of the two registration tables are build options: how
 * many sinks and how many topics one build registers depends on which
 * components a product selects, never on the board. The ring size and the
 * reader task are run-time configuration (xiaotai_eventlog_config_t in
 * xiaotai/eventlog.h), set per product.
 *
 * Both tables are allocated from XIAOTAI_MEM_BULK by xiaotai_eventlog_init()
 * and freed by xiaotai_eventlog_reset(). Registering beyond a capacity is
 * XIAOTAI_ERR_NO_MEM in the registering call (a topic handle is then NULL),
 * so a product whose components bring more raises the option, and no header
 * changes. */
#include "xiaotai/config.h"

/* Most sinks one build registers (xiaotai_eventlog_sink_register): the
 * console's follow sink today, later a persistent store or an uploader. */
#ifdef CONFIG_XIAOTAI_EVENTLOG_SINKS_MAX
#define XIAOTAI_EVENTLOG_SINKS_MAX CONFIG_XIAOTAI_EVENTLOG_SINKS_MAX
#else
#define XIAOTAI_EVENTLOG_SINKS_MAX 4
#endif

/* Most topics one build registers (xiaotai_eventlog_topic_register): one per
 * real-time report of the selected engines (media, transport, ...). */
#ifdef CONFIG_XIAOTAI_EVENTLOG_TOPICS_MAX
#define XIAOTAI_EVENTLOG_TOPICS_MAX CONFIG_XIAOTAI_EVENTLOG_TOPICS_MAX
#else
#define XIAOTAI_EVENTLOG_TOPICS_MAX 16
#endif
