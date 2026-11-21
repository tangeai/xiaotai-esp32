// SPDX-License-Identifier: MIT
#pragma once
/* Build options of the port layer (xiaotai/config.h explains the scheme). On
 * ESP-IDF the values come from this component's Kconfig; elsewhere the
 * defaults below apply, and a host test overrides one on the compiler command
 * line (-DCONFIG_XIAOTAI_KV_SLOTS=2).
 *
 * They are capacities that cost internal RAM on boards without external RAM,
 * so such a product sets them from measurements (the worker's lowest free
 * stack in xiaotai_task_list, the "full" count of xiaotai_kv_stats) instead of
 * paying for the largest product's needs. */
#include "xiaotai/config.h"

#ifdef CONFIG_XIAOTAI_PORT_MQTT_PAYLOAD_ON_DEMAND
#define XIAOTAI_PORT_MQTT_PAYLOAD_ON_DEMAND CONFIG_XIAOTAI_PORT_MQTT_PAYLOAD_ON_DEMAND
#else
#define XIAOTAI_PORT_MQTT_PAYLOAD_ON_DEMAND 0
#endif

#define XIAOTAI_PORT_HTTP_DNS_ON_DEMAND XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_PORT_HTTP_DNS_ON_DEMAND, 0)

#if defined(CONFIG_XIAOTAI_PORT_USB_MONITOR_FIX)
#define XIAOTAI_PORT_USB_MONITOR_FIX CONFIG_XIAOTAI_PORT_USB_MONITOR_FIX
#else
#define XIAOTAI_PORT_USB_MONITOR_FIX 0
#endif

#if defined(CONFIG_XIAOTAI_PORT_USB_TIMING_PROBE)
#define XIAOTAI_PORT_USB_TIMING_PROBE CONFIG_XIAOTAI_PORT_USB_TIMING_PROBE
#else
#define XIAOTAI_PORT_USB_TIMING_PROBE 0
#endif

#ifdef CONFIG_XIAOTAI_PORT_HEAP_TRACE_RECORDS
#define XIAOTAI_PORT_HEAP_TRACE_RECORDS CONFIG_XIAOTAI_PORT_HEAP_TRACE_RECORDS
#else
#define XIAOTAI_PORT_HEAP_TRACE_RECORDS 640U
#endif

/* ------------------------------------------------------------------ storage */

/* Job slots of the storage worker (2..8). Each costs XIAOTAI_KV_SLOT_BYTES
 * plus about 140 bytes (the names of up to XIAOTAI_KV_READS_MAX entries of a
 * read with a completion among them) of XIAOTAI_MEM_BULK for the life of the
 * program, and
 * one binary semaphore. A synchronous caller holds at most one slot while it
 * waits and asynchronous writes to one key share one, so the count is the
 * most concurrent callers plus the distinct keys written asynchronously at
 * once. When none is free a call fails with XIAOTAI_ERR_NO_MEM and counts in
 * xiaotai_kv_stats().full: a non-zero count means this value is too small. */
#ifdef CONFIG_XIAOTAI_KV_SLOTS
#define XIAOTAI_KV_SLOTS CONFIG_XIAOTAI_KV_SLOTS
#else
#define XIAOTAI_KV_SLOTS 4
#endif

/* Stack of the storage worker in bytes (2560..8192), always internal RAM:
 * the worker runs the flash writes, during which the cache may be off. The
 * worker exists from xiaotai_kv_init for the life of the program. */
#ifdef CONFIG_XIAOTAI_KV_WORKER_STACK_BYTES
#define XIAOTAI_KV_WORKER_STACK_BYTES CONFIG_XIAOTAI_KV_WORKER_STACK_BYTES
#else
#define XIAOTAI_KV_WORKER_STACK_BYTES 4096
#endif
