// SPDX-License-Identifier: MIT
#pragma once
/* Build options of the network engine (xiaotai/config.h explains the scheme).
 * On ESP-IDF the values come from this component's Kconfig; elsewhere the
 * defaults below apply, and a host test overrides one on the compiler command
 * line (-DCONFIG_XIAOTAI_NET_SCAN_MAX=4).
 *
 * These are capacities, timings and switches that hold for a whole build.
 * What differs per product at run time (storage layout, provisioning mode,
 * retry policy) is xiaotai_net_config_t in xiaotai/net.h; what differs per
 * board is the network part's descriptor. */
#include "xiaotai/config.h"

/* ---------------------------------------------------------------- capacities */

/* Networks the engine keeps from one scan (4..32). Kconfig default: 24 with
 * external RAM, 16 without. */
#ifdef CONFIG_XIAOTAI_NET_SCAN_MAX
#define XIAOTAI_NET_SCAN_MAX CONFIG_XIAOTAI_NET_SCAN_MAX
#else
#define XIAOTAI_NET_SCAN_MAX 24
#endif

/* Driver events and requests that may wait for the engine's worker (8..64). */
#ifdef CONFIG_XIAOTAI_NET_QUEUE_DEPTH
#define XIAOTAI_NET_QUEUE_DEPTH CONFIG_XIAOTAI_NET_QUEUE_DEPTH
#else
#define XIAOTAI_NET_QUEUE_DEPTH 16
#endif

/* Stack of the engine's worker, in bytes, internal RAM: it runs driver calls
 * that touch flash, the part's bring-up hook, and the immediate restart after
 * a coprocessor update or rollback, which the engine runs on an internal
 * stack on every board (xiaotai/net.h, Restarts). Kconfig default: 4096, and
 * 8192 when Wi-Fi is on a coprocessor (its bring-up verifies and writes
 * firmware images); the host uses the plain default. */
#ifdef CONFIG_XIAOTAI_NET_WORKER_STACK_BYTES
#define XIAOTAI_NET_WORKER_STACK_BYTES CONFIG_XIAOTAI_NET_WORKER_STACK_BYTES
#else
#define XIAOTAI_NET_WORKER_STACK_BYTES 4096
#endif

/* Stack of the setup page's server task, XIAOTAI_MEM_BULK. Kconfig default:
 * 8192 with external RAM, 7168 without. */
#ifdef CONFIG_XIAOTAI_NET_PORTAL_STACK_BYTES
#define XIAOTAI_NET_PORTAL_STACK_BYTES CONFIG_XIAOTAI_NET_PORTAL_STACK_BYTES
#else
#define XIAOTAI_NET_PORTAL_STACK_BYTES 8192
#endif

/* Stack of the captive name-server task, XIAOTAI_MEM_BULK. */
#ifdef CONFIG_XIAOTAI_NET_DNS_STACK_BYTES
#define XIAOTAI_NET_DNS_STACK_BYTES CONFIG_XIAOTAI_NET_DNS_STACK_BYTES
#else
#define XIAOTAI_NET_DNS_STACK_BYTES 4096
#endif

/* Largest coprocessor firmware image the console's update command downloads,
 * in bytes (XIAOTAI_MEM_BULK, only while an update runs). */
#ifdef CONFIG_XIAOTAI_NET_COPROCESSOR_IMAGE_MAX
#define XIAOTAI_NET_COPROCESSOR_IMAGE_MAX CONFIG_XIAOTAI_NET_COPROCESSOR_IMAGE_MAX
#else
#define XIAOTAI_NET_COPROCESSOR_IMAGE_MAX 0x180000
#endif

/* -------------------------------------------------------------------- timings */

/* Consecutive failures of the stored network after which status.failing is set
 * and, in concurrent mode, provisioning opens (reason FAILING). */
#ifdef CONFIG_XIAOTAI_NET_FAILURES_NOTICE
#define XIAOTAI_NET_FAILURES_NOTICE CONFIG_XIAOTAI_NET_FAILURES_NOTICE
#else
#define XIAOTAI_NET_FAILURES_NOTICE 5
#endif

/* Joined without an address for this long ends the attempt
 * (XIAOTAI_NET_LOSS_ADDRESS); 0: wait without limit. */
#ifdef CONFIG_XIAOTAI_NET_ADDRESS_TIMEOUT_MS
#define XIAOTAI_NET_ADDRESS_TIMEOUT_MS CONFIG_XIAOTAI_NET_ADDRESS_TIMEOUT_MS
#else
#define XIAOTAI_NET_ADDRESS_TIMEOUT_MS 30000
#endif

/* Shortest time between the starts of two scans. */
#ifdef CONFIG_XIAOTAI_NET_SCAN_INTERVAL_MS
#define XIAOTAI_NET_SCAN_INTERVAL_MS CONFIG_XIAOTAI_NET_SCAN_INTERVAL_MS
#else
#define XIAOTAI_NET_SCAN_INTERVAL_MS 5000
#endif

/* A scan that has not ended after this long is cancelled (state ERROR,
 * XIAOTAI_ERR_TIMEOUT). */
#ifdef CONFIG_XIAOTAI_NET_SCAN_TIMEOUT_MS
#define XIAOTAI_NET_SCAN_TIMEOUT_MS CONFIG_XIAOTAI_NET_SCAN_TIMEOUT_MS
#else
#define XIAOTAI_NET_SCAN_TIMEOUT_MS 10000
#endif

/* Background signal sampling period while the link is up. */
#ifdef CONFIG_XIAOTAI_NET_SIGNAL_INTERVAL_MS
#define XIAOTAI_NET_SIGNAL_INTERVAL_MS CONFIG_XIAOTAI_NET_SIGNAL_INTERVAL_MS
#else
#define XIAOTAI_NET_SIGNAL_INTERVAL_MS 30000
#endif

/* While UP without a station MAC (a remote read on a coprocessor failed),
 * the worker reads it again at this interval (xiaotai_net_status_t.mac). */
#ifdef CONFIG_XIAOTAI_NET_MAC_RETRY_MS
#define XIAOTAI_NET_MAC_RETRY_MS CONFIG_XIAOTAI_NET_MAC_RETRY_MS
#else
#define XIAOTAI_NET_MAC_RETRY_MS 5000
#endif

/* After a coprocessor rollback command succeeded, the longest the worker
 * waits for the caller to report and release the restart
 * (xiaotai_net_coprocessor_restart_now) before it restarts anyway. A
 * one-line console reply takes a few milliseconds; the bound only matters
 * for a caller that never releases. */
#ifdef CONFIG_XIAOTAI_NET_COPROCESSOR_REPLY_WAIT_MS
#define XIAOTAI_NET_COPROCESSOR_REPLY_WAIT_MS CONFIG_XIAOTAI_NET_COPROCESSOR_REPLY_WAIT_MS
#else
#define XIAOTAI_NET_COPROCESSOR_REPLY_WAIT_MS 1000
#endif

/* Verification of submitted credentials: the whole try is limited to the
 * timeout, and a loss during it is retried this many times. */
#ifdef CONFIG_XIAOTAI_NET_VERIFY_TIMEOUT_MS
#define XIAOTAI_NET_VERIFY_TIMEOUT_MS CONFIG_XIAOTAI_NET_VERIFY_TIMEOUT_MS
#else
#define XIAOTAI_NET_VERIFY_TIMEOUT_MS 30000
#endif
#ifdef CONFIG_XIAOTAI_NET_VERIFY_RETRIES
#define XIAOTAI_NET_VERIFY_RETRIES CONFIG_XIAOTAI_NET_VERIFY_RETRIES
#else
#define XIAOTAI_NET_VERIFY_RETRIES 3
#endif

/* --------------------------------------------------------------------- values */

/* Name server (dotted IPv4) the driver writes into the last slot where the
 * product's rule (xiaotai_net_config_t.dns_fallback in xiaotai/net.h) allows
 * it; "" disables it whatever the rule. */
#ifdef CONFIG_XIAOTAI_NET_FALLBACK_DNS_IPV4
#define XIAOTAI_NET_FALLBACK_DNS_IPV4 CONFIG_XIAOTAI_NET_FALLBACK_DNS_IPV4
#else
#define XIAOTAI_NET_FALLBACK_DNS_IPV4 "223.5.5.5"
#endif

/* ------------------------------------------------------------------- switches */

/* Compile the setup access point with its page and captive name server
 * (XIAOTAI_PROVISION_PORTAL). Off: the code and the page are not in the image. */
#define XIAOTAI_NET_PROVISION_PORTAL XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_NET_PROVISION_PORTAL, 1)

/* Compile the serial provisioning commands (XIAOTAI_PROVISION_SERIAL). */
#define XIAOTAI_NET_PROVISION_SERIAL XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_NET_PROVISION_SERIAL, 1)

/* Console commands that erase the stored network: AT+NET FORGET of this
 * engine and the FORGET word of the provisioning module's AT+NET PROVISION.
 * Off: neither is registered, so a production console cannot clear Wi-Fi
 * (deployed production consoles cannot either); development builds turn it
 * on. The engine's xiaotai_net_forget_credentials and xiaotai_net_reprovision
 * are not affected. */
#define XIAOTAI_NET_CONSOLE_FORGET XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_NET_CONSOLE_FORGET, 0)

/* Diagnostic build: local console authorizes a single deterministic UDP run. */
#define XIAOTAI_NET_PATTERN_PROBE XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_NET_PATTERN_PROBE, 0)

/* Diagnostic build: the packet path capture (xiaotai_net_trace_*) and its
 * console commands. Costs XIAOTAI_NET_PATH_TRACE_SECONDS rows of counters in
 * XIAOTAI_MEM_BULK and wraps two functions of the network stack. */
#define XIAOTAI_NET_PATH_TRACE XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_NET_PATH_TRACE, 0)

/* Longest capture, one row per second. */
#ifdef CONFIG_XIAOTAI_NET_PATH_TRACE_SECONDS
#define XIAOTAI_NET_PATH_TRACE_SECONDS CONFIG_XIAOTAI_NET_PATH_TRACE_SECONDS
#else
#define XIAOTAI_NET_PATH_TRACE_SECONDS 1800
#endif

/* With the capture: the transport also records its statistics samples
 * (xiaotai_net_trace_transport_active). Its statistics logging perturbs
 * weak-network timing, so it is a separate choice. */
#define XIAOTAI_NET_PATH_TRACE_TRANSPORT XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_NET_PATH_TRACE_TRANSPORT, 0)

/* ----------------------------------------------------------------- constants */

/* Facts the setup page, the on-screen instructions and the request checks all
 * rely on; not options. */
#define XIAOTAI_NET_AP_IPV4 "192.168.6.1"
#define XIAOTAI_NET_AP_URL "http://192.168.6.1"
#define XIAOTAI_NET_AP_CHANNEL 1
#define XIAOTAI_NET_AP_CLIENTS_MAX 4
