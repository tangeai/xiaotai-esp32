// SPDX-License-Identifier: MIT
#pragma once
/* Network drivers: the code that operates one kind of link hardware for the
 * network engine (xiaotai/net.h).
 *
 * The board lists network parts with a kind (xiaotai/hal.h); a driver is
 * registered per kind. The engine binds the driver of the kind to the part and
 * from then on is its only caller. A new network kind is one more driver: no
 * function of the engine and nothing in this file changes.
 *
 * Where drivers come from. The two Wi-Fi kinds are built into this engine,
 * and its manifest declares that it drives them ("network_kinds": "wifi",
 * "wifi_coprocessor"). Any other kind is a driver component of its own (an
 * engine in the manifest sense, depending on "net") whose manifest declares
 * the kinds it drives the same way and which calls
 * xiaotai_net_driver_register from its init. For every network kind the
 * board's manifest lists, the build selects the component whose manifest
 * declares that kind and fails before compiling when none does, so a product
 * never names a driver.
 *
 * Board facts a driver needs beyond the kind (an Ethernet PHY address and
 * pins, a modem's UART and power key, how a coprocessor's buffers are
 * reserved) are a struct the board's network part points to
 * (xiaotai_network_t.driver_config); the driver reads it in start() and
 * takes the struct's defaults, all zero, when the part gives none. Board
 * packages sit below the engines and cannot include a driver's header, so
 * each kind's struct is defined at the hardware abstraction's level
 * (xiaotai/hal.h for the Wi-Fi kinds, the kind's part library header
 * otherwise), where both the board and the driver see it. The two Wi-Fi
 * drivers read xiaotai_network_wifi_t: with tx_buffers_on_demand the
 * coprocessor driver has the coprocessor allocate its transmit buffers on
 * demand and keep none cached. Exactly: when the built configuration uses
 * static transmit buffers, their count becomes the limit of dynamic ones, the
 * type becomes dynamic and the static count 0; when it already uses dynamic
 * buffers, their count and the type stay as built; in both cases the cached
 * transmit buffers become 0. Without it the coprocessor's buffer
 * configuration stays as built. A driver decides by these facts only, never
 * by a board, chip or product name or a platform symbol naming one.
 *
 * Division of work. The driver knows the hardware and the platform's network
 * stack: interfaces, radio parameters, address assignment, the setup access
 * point with its address server. It holds no policy: when to join, how long to
 * wait, when to give up, what to store and when to open provisioning are the
 * engine's. Bring-up of hardware that has firmware of its own is the part's
 * prepare() hook, which the engine runs before start().
 *
 * Calls. Every function below is called on the engine's worker task, one at a
 * time, never from the framework owner task. A function may block as long as
 * the hardware needs (on a coprocessor every call is a remote call), up to
 * XIAOTAI_NET_DRIVER_CALL_MAX_MS; it returns only when the request is in
 * effect or has failed. A NULL function pointer means the driver lacks the
 * capability; pointers marked required are never NULL.
 *
 * Events. What happens later is reported through the emit function given to
 * start(), from whatever task the platform delivers it on. emit copies the
 * event into the engine's queue and returns: it never blocks, allocates or
 * logs, and may be called while a driver function is running. Events that
 * answer a request carry the token of that request; the engine drops events
 * whose token is not the current one and counts them.
 *
 * Memory is requested by purpose (xiaotai/port/mem.h); scan records and other
 * bulk data go to XIAOTAI_MEM_BULK. Credentials given to join() are not kept
 * after the link is left and never logged. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/framework.h"
#include "xiaotai/hal.h"
#include "xiaotai/net.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Longest any driver function may block. */
#define XIAOTAI_NET_DRIVER_CALL_MAX_MS 10000U

/* What a driver can do, beyond bringing a link up. */
enum {
    /* join() takes a network name and password (a Wi-Fi station). Without it
     * the link needs no credentials: join() is called with NULL and the engine
     * offers no provisioning for it. */
    XIAOTAI_NET_DRIVER_CREDENTIALS = 1u << 0,
    /* scan(), scan_cancel() and scan_read() are present. */
    XIAOTAI_NET_DRIVER_SCAN = 1u << 1,
    /* ap_start() and ap_stop() are present, and the access point works while
     * the station side joins, is joined or is idle. */
    XIAOTAI_NET_DRIVER_ACCESS_POINT = 1u << 2,
    /* signal() is present. */
    XIAOTAI_NET_DRIVER_SIGNAL = 1u << 3,
};

/* -------------------------------------------------------------------- events */

typedef enum {
    /* The link layer is up (a station associated). ssid holds the bytes of the
     * network actually joined, so the engine can tell it from the one asked for. */
    XIAOTAI_NET_DRIVER_JOINED = 0,
    /* The current addresses: ipv4 and ipv6 when ipv6_valid. An address is
     * published after its setup (see fallback_dns), so name resolution works
     * at once. Both fields empty after a published address mean it was lost
     * but the association may still exist. Keep the same token until LOST;
     * the worker may call leave() without first attempting another join(). */
    XIAOTAI_NET_DRIVER_ADDRESS,
    /* The attempt failed, or the link or its address was lost: loss, code,
     * signal. Also the answer to leave() (loss XIAOTAI_NET_LOSS_REQUESTED).
     * Exactly one LOST ends every join() that returned XIAOTAI_OK. The driver
     * maps the platform's reason to loss by what it means: the network not
     * seen at all is NOT_FOUND; seen without an acceptable security is
     * SECURITY; authentication and handshake failures are CREDENTIALS (the
     * mapping of the ESP-IDF reasons is in the component's specification). */
    XIAOTAI_NET_DRIVER_LOST,
    /* The scan with this token ended: error is XIAOTAI_OK when scan_read() has
     * results. A scan ended by scan_cancel() emits nothing. */
    XIAOTAI_NET_DRIVER_SCAN_DONE,
    /* The hardware stopped on its own (the coprocessor restarted, the interface
     * went away). The engine makes no attempt until STARTED. token is 0. */
    XIAOTAI_NET_DRIVER_STOPPED,
    /* The hardware is usable again after STOPPED. token is 0. */
    XIAOTAI_NET_DRIVER_STARTED,
} xiaotai_net_driver_event_kind_t;

typedef struct {
    xiaotai_net_driver_event_kind_t kind;
    /* Token of the join() (JOINED, ADDRESS, LOST) or scan() (SCAN_DONE) the
     * event belongs to. */
    uint32_t token;
    xiaotai_err_t error;                  /* SCAN_DONE */
    xiaotai_net_loss_t loss;              /* LOST */
    int32_t code;                         /* LOST: the platform's own reason number */
    bool signal_valid;                    /* LOST: signal_dbm was sampled when it happened */
    int8_t signal_dbm;
    uint32_t ipv4;                        /* ADDRESS: the four address bytes in memory order; 0: none */
    char ssid[XIAOTAI_NET_SSID_MAX + 1];  /* JOINED */
    bool ipv6_valid;                      /* ADDRESS: a global IPv6 address */
    uint8_t ipv6[16];
    uint32_t gateway_ipv4;                 /* ADDRESS: associated gateway, same byte order, 0: not reported */
    uint8_t channel;                       /* JOINED: primary channel, 0: not reported */
} xiaotai_net_driver_event_t;

/* [any task, never an interrupt] See "Events" above. */
typedef void (*xiaotai_net_driver_emit_fn_t)(void *ctx, const xiaotai_net_driver_event_t *event);

/* ------------------------------------------------------------------- requests */

typedef struct {
    /* Name server (dotted IPv4) to put into the last name-server slot by
     * fallback_dns_rule, before ADDRESS is emitted; "" or NULL: leave the
     * slots alone. Slots the network filled are never changed. Static
     * storage. */
    const char *fallback_dns;
    xiaotai_net_driver_emit_fn_t emit;
    void *emit_ctx;
    /* The product's rule (xiaotai_net_config_t.dns_fallback). Which slots it
     * reads and whether it writes is decided by the engine's portable
     * function; the driver only reads and writes the slots. A failed read or
     * write is counted and ADDRESS is emitted all the same. */
    xiaotai_net_dns_fallback_t fallback_dns_rule;
    /* xiaotai_net_config_t.driver_storage: false turns the platform driver's
     * own persistent storage of the station configuration off in start(),
     * before the platform driver is configured; true leaves the platform's
     * default. */
    bool driver_storage;
} xiaotai_net_driver_start_t;

enum {
    /* A network with a password must offer WPA2 or better; an attempt on a
     * weaker one ends with LOST (XIAOTAI_NET_LOSS_SECURITY). */
    XIAOTAI_NET_JOIN_REQUIRE_WPA2 = 1u << 0,
};

typedef struct {
    char ssid[XIAOTAI_NET_SSID_MAX + 1];   /* hidden networks (no name) are left out by the driver */
    int8_t signal_dbm;
    uint8_t channel;                       /* 0: unknown */
    xiaotai_net_security_t security;
} xiaotai_net_scan_record_t;

/* The setup access point. Its address is also the gateway and the name server
 * its address server hands out, with a /24 mask. The address server must not
 * advertise a captive-portal URL (DHCP option 114): phones that receive one
 * expect an HTTPS API there and then never show the setup page. */
typedef struct {
    char ssid[XIAOTAI_NET_SSID_MAX + 1];
    char password[XIAOTAI_NET_PASSWORD_MAX + 1];   /* "": open */
    uint8_t channel;
    uint8_t max_clients;
    uint32_t ipv4;                                  /* the four address bytes in memory order */
} xiaotai_net_ap_config_t;

/* -------------------------------------------------------------------- driver */

/* The struct and what it points to have static storage duration. */
typedef struct xiaotai_net_driver {
    xiaotai_network_kind_t kind;
    const char *name;              /* for diagnostics: [a-z0-9_], at most 15 characters */
    uint32_t capabilities;         /* XIAOTAI_NET_DRIVER_* */

    /* Required. Brings the platform's network stack and the hardware up for
     * this part, after its prepare() hook passed; the board's facts for the
     * driver come with the part (see "Board facts" above; a part without
     * them gets the struct's defaults, all zero). Returns when join(), scan()
     * and ap_start() may be called and mac() answers. Configuration that must
     * precede the first attempt (power saving off, protocol set, channel
     * width, the driver-side storage as start->driver_storage says, the
     * coprocessor's buffers as the part's facts say) is applied here; a step
     * that fails makes start() fail. Called once per boot; a failed start()
     * may be repeated (xiaotai_net_retry) and must have undone what it began. */
    xiaotai_err_t (*start)(void *self, const xiaotai_network_t *part,
                           const xiaotai_net_driver_start_t *start);

    /* Required. Begins one attempt to bring the link up; JOINED, ADDRESS and
     * LOST events carry token (never 0). credentials is NULL without
     * XIAOTAI_NET_DRIVER_CREDENTIALS, otherwise valid by
     * xiaotai_net_credentials_valid or, when read from storage of the plain
     * layout, by its read rule (a password of 1..7 bytes is possible and is
     * passed through unchanged); borrowed for the call. flags:
     * XIAOTAI_NET_JOIN_*. The driver does not retry by itself. A result other
     * than XIAOTAI_OK means nothing was begun and no event follows; the engine
     * records the refusal (XIAOTAI_NET_LOSS_REFUSED). Never called
     * while an attempt or a link exists (the engine calls leave() first and
     * waits for its LOST) and never while a scan runs. */
    xiaotai_err_t (*join)(void *self, uint32_t token, const xiaotai_net_credentials_t *credentials,
                          uint32_t flags);

    /* Optional. Classifies a synchronous join error as a stopped radio, not
     * a refused association. Preserve the original error for the caller. */
    bool (*join_radio_unavailable)(xiaotai_err_t result);

    /* Required. Ends the attempt or leaves the link. XIAOTAI_OK: a LOST with
     * XIAOTAI_NET_LOSS_REQUESTED follows (or one with another loss that was
     * already on its way). The worker may repeat the request for the same
     * outstanding join without producing a second terminal event.
     * XIAOTAI_ERR_INVALID_STATE: no attempt or link remains. Other errors do
     * not establish that the link is gone. */
    xiaotai_err_t (*leave)(void *self);

    /* XIAOTAI_NET_DRIVER_SCAN. Begins a scan of every channel; SCAN_DONE
     * carries token (never 0). Never called while an attempt is in flight. */
    xiaotai_err_t (*scan)(void *self, uint32_t token);
    /* Stops the running scan. No SCAN_DONE follows for its token; a completion
     * the platform still delivers is swallowed by the driver. */
    xiaotai_err_t (*scan_cancel)(void *self);
    /* After SCAN_DONE with XIAOTAI_OK: copies up to capacity records, in any
     * order, and releases the platform's result list. *count: records copied;
     * *truncated: more were found than capacity. Results are read once. */
    xiaotai_err_t (*scan_read)(void *self, xiaotai_net_scan_record_t *records, size_t capacity,
                               size_t *count, bool *truncated);

    /* XIAOTAI_NET_DRIVER_ACCESS_POINT. Starts the setup access point with its
     * address server. When it returns XIAOTAI_OK, sockets can be bound to
     * config->ipv4. On failure the driver attempts rollback; the engine
     * confirms teardown with ap_stop() before retrying. The station side
     * keeps its state. */
    xiaotai_err_t (*ap_start)(void *self, const xiaotai_net_ap_config_t *config);
    /* Stops the access point; sockets bound to its address fail from then on.
     * The station side keeps its state. Stopping a stopped one is XIAOTAI_OK. */
    xiaotai_err_t (*ap_stop)(void *self);

    /* XIAOTAI_NET_DRIVER_SIGNAL. Signal strength of the present link.
     * XIAOTAI_ERR_INVALID_STATE without a link. */
    xiaotai_err_t (*signal)(void *self, int8_t *dbm);

    /* Hardware address of the station side, the one the device is known by.
     * On a coprocessor it is the coprocessor's. Valid after start().
     * NULL: the link has no hardware address of its own. */
    xiaotai_err_t (*mac)(void *self, uint8_t mac[6]);

    /* Diagnostics: writes lines about the present link (power saving, channel
     * width, protocol, channel, signal, stack and bus counters) through out.
     * Reads back from the hardware; may block. */
    void (*report)(void *self, const xiaotai_reply_t *out);

    void *self;
} xiaotai_net_driver_t;

/* [init] of a driver component (or earlier, from a test); not once the engine
 * started. One driver per kind. Every init runs before any start, so a driver
 * registered by another engine's init is in place when this engine binds. The
 * engine's own init registers the platform's built-in drivers (the chip's own
 * Wi-Fi, Wi-Fi on a coprocessor) only for kinds that have none, so a
 * driver component or a test replaces one by registering first.
 * XIAOTAI_ERR_INVALID_ARG: NULL, a missing required function, or a capability
 * bit without its functions. XIAOTAI_ERR_INVALID_STATE: the kind is taken, or
 * the engine started. */
xiaotai_err_t xiaotai_net_driver_register(const xiaotai_net_driver_t *driver);

/* [any] The driver bound to the part in use; NULL before the engine started or
 * when no part has a driver. For diagnostics: the functions of the returned
 * driver belong to the engine's worker. */
const xiaotai_net_driver_t *xiaotai_net_driver_bound(void);

#ifdef __cplusplus
}
#endif
