#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "tiRTC.h"
typedef enum {
    RTC_STARTED,
    RTC_STOPPED,
    RTC_CONNECTED,
    RTC_ERROR,
    RTC_CLOSED,
    RTC_COMMAND,
    RTC_SUBSCRIBE,
    RTC_UNSUBSCRIBE,
    RTC_UNEXPECTED
} rtc_event_kind_t;
typedef struct {
    rtc_event_kind_t kind;
    uint32_t generation;
    tirtc_conn_t connection;
    int error;
    uint32_t command;
    uint32_t length;
    uint8_t stream;
    char data[96];
} rtc_event_t;
typedef bool (*rtc_observer_t)(const rtc_event_t *);
typedef void (*rtc_audio_receiver_t)(tirtc_conn_t, const TIRTCFRAMEINFO *, const void *);
void rtc_adapter_init(rtc_observer_t observer, rtc_audio_receiver_t receiver);
/* Lifecycle calls belong to the control task. Register the observer first:
 * SYS_STARTED/SYS_STOPPED may be emitted before start/stop returns. */
int rtc_start(const char *device_id, const char *key, const char *client_id, const char *endpoint);
/* Single-flight: the control task must finish the preceding connection first.
 * Copies both inputs before submitting. They can be released after this returns.
 * Zero means submitted, not connected; wait for RTC_CONNECTED and VoIP 0x2000. */
int rtc_connect(uint32_t generation, const char *peer, const char *token);
/* Release the owned async WHIP inputs after RTC_CLOSED or a failed
 * RTC_CONNECTED result. The capacity limits remain 2048/1024 bytes. */
void rtc_connect_inputs_release(void);
/* Retire the handle before calling; RTC_CLOSED is only a completion identity. */
int rtc_disconnect(tirtc_conn_t connection);
int rtc_stop(void);
/* Call after the queued RTC_STOPPED event, never inside an SDK callback. */
void rtc_uninit(void);
/* Send APIs preserve the SDK result: positive byte count or negative error. */
int rtc_send_command(tirtc_conn_t connection, uint32_t command, const char *json);
/* SDK 2.5.0 synchronous request: zero is completed success, nonzero is failure.
 * Optional callback carries data (not the response envelope), not completion.
 * Called on the control task; passes NULL as the device-authenticated token. */
int rtc_service_request(const char *path, const char *json, TIRTCSERVICEREQUESTCALLBACK callback);
int rtc_send_audio(tirtc_conn_t connection, const TIRTCFRAMEINFO *frame, const void *data);
size_t rtc_send_buffer_used(tirtc_conn_t connection);
