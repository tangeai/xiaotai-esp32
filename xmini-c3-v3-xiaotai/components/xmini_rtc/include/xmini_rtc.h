#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tiRTC.h"
#include "tgtrp_internal_diag.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XMINI_RTC_PCMA_BYTES 160
#define XMINI_RTC_RX_BLOCKS_PER_CALLBACK 8U
#define XMINI_RTC_COMMAND_BYTES 512
#define XMINI_RTC_COMMAND_MAX_BYTES (12U * 1024U)
#define XMINI_RTC_COMMAND_BUDGET (16U * 1024U)

typedef enum {
    XMINI_RTC_STARTED,
    XMINI_RTC_STOPPED,
    XMINI_RTC_CONNECTED,
    XMINI_RTC_ACCEPTED,
    XMINI_RTC_ERROR,
    XMINI_RTC_CLOSED,
    XMINI_RTC_COMMAND,
    XMINI_RTC_MESSAGE,
    XMINI_RTC_SUBSCRIBE,
    XMINI_RTC_UNSUBSCRIBE,
    XMINI_RTC_REJECTED,
    XMINI_RTC_OVERFLOW,
    XMINI_RTC_FORMAT_ERROR,
    XMINI_RTC_ACCESS_HIJACKING,
    XMINI_RTC_RETIRED
} xmini_rtc_event_kind_t;

typedef enum { XMINI_RTC_P2P, XMINI_RTC_WHIP } xmini_rtc_mode_t;

typedef struct {
    xmini_rtc_event_kind_t kind;
    uint32_t generation;
    tirtc_conn_t connection; /* Identity only after CLOSED; never dereference. */
    int error;
    uint32_t command; /* Exact wire word; legacy business IDs are bare words. */
    uint32_t command_id; /* GET_CMD extraction, meaningful for packed protocols. */
    uint16_t command_sequence;
    bool command_response;
    uint32_t length;
    uint8_t stream;
    bool retired; /* True only under the SDK's verified pending-close contract. */
    char data[XMINI_RTC_COMMAND_BYTES];
    char *payload; /* Optional long payload; the poll consumer must release it. */
} xmini_rtc_event_t;

/* SDK callback context: copy/enqueue one complete 160B/20ms block before returning.
 * Arbitrary nonempty PCMA chunks are reblocked in byte order for the single owned
 * receive subscription. PTS annotates the first source byte, then advances 20ms
 * within that callback; upstream wall-clock timestamps are not a sample clock.
 * Without a protocol PTT boundary, a partial tail survives a pause. Never block. */
typedef bool (*xmini_rtc_receiver_t)(uint32_t generation, uint8_t stream, uint32_t pts,
                                    const uint8_t *pcma, size_t length, void *context);

typedef struct {
    bool initialized, online, connecting, connected, closing, stopping, faulted;
    uint32_t generation;
    uint16_t subscribed, business_permit;
    uint32_t tx_frames, rx_frames, tx_dropped, rx_dropped, stale_frames;
    uint32_t command_oversize, event_overflow, format_errors;
    int last_error;
    unsigned events_pending, tx_pending, sdk_pending;
    size_t command_bytes_pending;
    uint32_t transport_capture_sequence;
    uint32_t rx_carry_bytes, rx_partial_dropped, rx_bytes_dropped, rx_unselected_frames;
    /* Last format-valid, current selected SDK input; retained across retirement.
     * Chunks restart on the first input of a different generation, not per block. */
    uint32_t rx_input_generation, rx_input_chunks, rx_input_length;
    uint8_t rx_input_media, rx_input_flags, rx_input_stream, rx_input_valid;
} xmini_rtc_status_t;

typedef struct {
    uint32_t capture_sequence;
    uint32_t generation;
    uintptr_t connection;
    tgtrp_internal_diag_t transport;
} xmini_rtc_transport_snapshot_t;

typedef struct {
    xmini_rtc_transport_snapshot_t tx, rx, queue;
} xmini_rtc_transport_diagnostics_t;

#define XMINI_RTC_COMMAND_DIAGNOSTIC_SLOTS 4U
enum {
    XMINI_RTC_CMD_ONLINE = 1U << 0, XMINI_RTC_CMD_CONNECTED = 1U << 1,
    XMINI_RTC_CMD_STOPPING = 1U << 2, XMINI_RTC_CMD_FAULTED = 1U << 3,
    XMINI_RTC_CMD_PENDING = 1U << 4, XMINI_RTC_CMD_CANCELLED = 1U << 5,
    XMINI_RTC_CMD_ARMED = 1U << 6, XMINI_RTC_CMD_CONNECTION_MATCH = 1U << 7,
    XMINI_RTC_CMD_GENERATION_MATCH = 1U << 8, XMINI_RTC_CMD_ACTIVE = 1U << 9,
    XMINI_RTC_CMD_SDK_PENDING = 1U << 10, XMINI_RTC_CMD_CLOSING_CONNECTION = 1U << 11,
    XMINI_RTC_CMD_RETIRING = 1U << 12,
    XMINI_RTC_CMD_QUEUED = 1U << 24,
};
typedef enum {
    XMINI_RTC_CMD_ENTRY, XMINI_RTC_CMD_INACTIVE, XMINI_RTC_CMD_INVALID_INPUT,
    XMINI_RTC_CMD_NO_MEMORY, XMINI_RTC_CMD_EVENT,
} xmini_rtc_command_disposition_t;
#define XMINI_RTC_CMD_DISPOSITION_SHIFT 16U
#define XMINI_RTC_CMD_DISPOSITION_MASK (0xffU << XMINI_RTC_CMD_DISPOSITION_SHIFT)
typedef struct {
    uint32_t sequence, at_ms, connection, command, length;
    uint32_t observed_generation, current_generation, pending_generation, inbound_generation, flags;
} xmini_rtc_command_diagnostic_t;
typedef struct { uint32_t captures, overwritten; } xmini_rtc_command_diagnostic_summary_t;
typedef struct {
    uint32_t sequence, at_ms, generation, connection, command, length;
    int32_t result;
    uint32_t flags; /* Bit0: SDK was called; bit1: sequence saturated. */
} xmini_rtc_command_tx_diagnostic_t;

/* init/start/connect/cancel/process/poll/send_command/subscribe/stop/uninit are
 * serialized by one control task. The 10 KiB diagnostic stack still needs full-path board coverage. */
int xmini_rtc_init(xmini_rtc_receiver_t receiver, void *context);
int xmini_rtc_start(const char *device_id, const char *key, const char *client_id,
                    const char *endpoint);
int xmini_rtc_arm_inbound(uint32_t generation, bool accept);
int xmini_rtc_connect(xmini_rtc_mode_t mode, uint32_t generation,
                      const char *peer, const char *token);
/* Pending cancellation retains async input copies until the result arrives.
 * A late success is retired on the control task, never becomes media-active. */
int xmini_rtc_cancel(uint32_t generation);
int xmini_rtc_disconnect(uint32_t generation);
/* Run at <=20 ms intervals on the control task, including during cancellation. */
void xmini_rtc_process(void);
bool xmini_rtc_poll(xmini_rtc_event_t *event);
const char *xmini_rtc_event_payload(const xmini_rtc_event_t *event);
void xmini_rtc_event_release(xmini_rtc_event_t *event);
/* Exact wire word, unchanged. The business protocol chooses bare or packed form.
 * Returns the SDK's positive sent-byte count, or a negative error. */
int xmini_rtc_send_command(uint32_t generation, uint32_t command,
                           const void *data, size_t length);
/* Normalize the SDK's >=0 subscription success to zero; preserve negative errors.
 * One playback subscription is owned at a time. A successful stream change or
 * matching unsubscribe discards its partial tail, without changing TX permission. */
int xmini_rtc_subscribe(uint32_t generation, uint8_t stream, bool subscribe);
/* Control owner only: admit one local playback stream for this connected
 * generation, without sending a network subscription or changing TX permission.
 * Re-selecting the same stream preserves its partial tail; changing/removing it
 * discards the tail and invalidates in-flight assembly through the same epoch. */
int xmini_rtc_select_audio(uint32_t generation, uint8_t stream, bool selected);
/* Explicit negotiated business permission, not a fabricated SDK subscription.
 * H5 must not use this API. Unsubscribe revokes it and invalidates queued audio. */
int xmini_rtc_authorize_audio(uint32_t generation, uint8_t stream, bool allow);
/* SDK 2.5.0 request is synchronous; zero is completed success. Callback, when
 * supplied, carries response data only and may not be called on empty success. */
int xmini_rtc_service_request(const char *path, const char *json,
                               TIRTCSERVICEREQUESTCALLBACK callback, void *context);
/* May be called by the audio task. Stream needs remote or negotiated business permission.
 * Copies exactly one 20 ms A-law packet; BUSY is observable bounded backpressure. */
int xmini_rtc_enqueue_audio(uint32_t generation, uint8_t stream,
                            const uint8_t *pcma, size_t length, uint32_t pts);
int xmini_rtc_stop(void);
/* Only after SYS_STOPPED (or start failed before SDK became active). */
int xmini_rtc_uninit(void);
void xmini_rtc_status(xmini_rtc_status_t *status);
/* Latest borrowed-hook copies only; zero capture_sequence means unavailable.
 * Generation/connection identify the application owner at capture time, while
 * transport.identity identifies the actual lower session. Never dereference
 * either identity. Records survive cleanup and must be compared by both IDs. */
void xmini_rtc_transport_diagnostics(xmini_rtc_transport_diagnostics_t *out);
/* Fixed metadata only, no payload or secret pointers. Physical ring slots are
 * individually coherent; callers must retain sequence/overwrite evidence.
 * UINT32_MAX captures/overwritten means coverage unavailable, not zero traffic.
 * Records survive session cleanup. Time and flags are sampled in the gate lock. */
bool xmini_rtc_command_diagnostic(unsigned slot, xmini_rtc_command_diagnostic_summary_t *summary,
                                   xmini_rtc_command_diagnostic_t *out);
/* Last raw 0x2000 submission only; SDK positive byte count is not an ACK.
 * Other command words do not overwrite this notice evidence. */
void xmini_rtc_command_tx_diagnostic(xmini_rtc_command_tx_diagnostic_t *out);

#ifdef __cplusplus
}
#endif
