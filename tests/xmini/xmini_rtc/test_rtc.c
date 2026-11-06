#include "xmini_rtc.h"
#include "xmini_rtc_limits.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include "freertos/queue.h"

struct _tirtc_conn { void *user; bool closed; };
static struct _tirtc_conn s_handle, s_handle2;
static const TIRTCCALLBACKS *s_callbacks;
static TIRTCCONNECTCALLBACK s_connect;
static void *s_user;
static const char *s_peer, *s_token;
static const char *s_expected_peer, *s_expected_token;
static unsigned s_disconnects, s_frames, s_receives, s_uninit, s_step, s_whip, s_p2p;
static size_t s_used;
static int s_submit_result, s_send_result = 172, s_start_result;
static int s_subscribe_result = 5, s_command_result = 6;
static uint32_t s_command_word;
static uint8_t s_pcma[160];
static void *s_task = (void *)1;
static int64_t s_now;
static char s_format_log[256], s_enqueue_log[256];
static unsigned s_format_logs;
static bool s_inspect_command_entry;
static unsigned s_command_entries;
static const uint8_t *s_receive_expected = s_pcma;
static uint32_t s_receive_pts = 20;
static unsigned s_receive_attempts, s_receive_capacity = 32;
static bool s_receive_connection_error;
#ifdef TIRTC_PENDING_CLOSE_CONTRACT
static unsigned s_sdk_pending;
unsigned TiRtcPendingConnections(void) { return s_sdk_pending; }
int TiRtcCancelPending(void *user, int inbound)
{ (void)user; assert(inbound == 1); return 0; }
#endif
int64_t esp_timer_get_time(void) { return s_now; }

QueueHandle_t xQueueCreateStatic(UBaseType_t capacity, UBaseType_t size, uint8_t *storage,
                                  StaticQueue_t *queue)
{
    *queue = (StaticQueue_t){.capacity = capacity, .size = size, .data = storage};
    return queue;
}
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, unsigned ticks)
{
    assert(ticks == 0);
    if (s_inspect_command_entry && queue->size == sizeof(xmini_rtc_event_t) &&
        ((const xmini_rtc_event_t *)item)->kind == XMINI_RTC_COMMAND) {
        xmini_rtc_command_diagnostic_summary_t summary;
        xmini_rtc_command_diagnostic_t row;
        xmini_rtc_command_diagnostic(0, &summary, NULL);
        assert(summary.captures && xmini_rtc_command_diagnostic((summary.captures - 1) % 4, NULL, &row));
        assert(row.sequence == summary.captures && (row.flags & XMINI_RTC_CMD_ACTIVE));
        assert((row.flags & XMINI_RTC_CMD_DISPOSITION_MASK) == 0 && !(row.flags & XMINI_RTC_CMD_QUEUED));
        ++s_command_entries;
    }
    if (queue->count == queue->capacity) return pdFALSE;
    memcpy(queue->data + ((queue->head + queue->count) % queue->capacity) * queue->size,
           item, queue->size);
    ++queue->count;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, unsigned ticks)
{
    assert(ticks == 0);
    if (!queue->count) return pdFALSE;
    memcpy(item, queue->data + queue->head * queue->size, queue->size);
    queue->head = (queue->head + 1) % queue->capacity;
    --queue->count;
    return pdTRUE;
}
BaseType_t xQueueReset(QueueHandle_t queue) { queue->head = queue->count = 0; return pdTRUE; }
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue) { return queue->count; }
void *xTaskGetCurrentTaskHandle(void) { return s_task; }
void test_log(const char *level, const char *tag, const char *format, ...)
{
    (void)level; (void)tag;
    va_list arguments;
    va_start(arguments, format);
    if (strstr(format, "RX FORMAT")) {
        vsnprintf(s_format_log, sizeof(s_format_log), format, arguments);
        ++s_format_logs;
    } else if (strstr(format, "TX ENQUEUE")) {
        vsnprintf(s_enqueue_log, sizeof(s_enqueue_log), format, arguments);
    }
    va_end(arguments);
}

int TiRtcSetOption(TIRTCOPTION option, const void *data, uint32_t length)
{
    switch (option) {
    case TIRTC_OPT_MAX_SEND_BUFFER:
        assert(s_step++ == 0 && length == sizeof(uint32_t) && *(const uint32_t *)data == 16384); break;
    case TIRTC_OPT_MAX_CONNECTIONS:
        assert(s_step++ == 1 && length == sizeof(int) && *(const int *)data == 1); break;
    case TIRTC_OPT_CONNECT_CACHE:
        assert(s_step++ == 2 && length == sizeof(int) && *(const int *)data == 0); break;
    case TIRTC_OPT_DEVICE_SECRET_KEY:
        assert(s_step++ == 4 && length == 3 && memcmp(data, "key", 3) == 0); break;
    case TIRTC_OPT_CLIENT_ID:
        assert(s_step++ == 5 && length == 6 && memcmp(data, "client", 6) == 0); break;
    case TIRTC_OPT_SERVICE_ENDPOINT:
        assert(s_step++ == 6 && length == 21 && memcmp(data, "http://sample.invalid", 21) == 0); break;
    default: assert(false);
    }
    return 0;
}
int TiRtcInit(void) { assert(s_step++ == 3); return 0; }
void TiRtcLogSetLevel(int level) { assert(level == 0); }
const char *TiRtcGetVersion(void) { return "2.5.0-test"; }
int TiRtcStart(const char *id, const TIRTCCALLBACKS *callbacks)
{
    assert(s_step++ == 7 && strcmp(id, "device") == 0);
    s_callbacks = callbacks;
    if (!s_start_result) callbacks->on_event(TIRTC_EVENT_SYS_STARTED, NULL, 0);
    return s_start_result;
}
int TiRtcStop(void) { s_callbacks->on_event(TIRTC_EVENT_SYS_STOPPED, NULL, 0); return 0; }
void TiRtcUninit(void) { ++s_uninit; }
static int connect_mock(const char *peer, const char *token, TIRTCCONNECTCALLBACK callback, void *user)
{
    assert(strcmp(peer, s_expected_peer ? s_expected_peer : "peer") == 0);
    assert(strcmp(token, s_expected_token ? s_expected_token : "token") == 0);
    s_peer = peer; s_token = token; s_connect = callback; s_user = user;
    return s_submit_result;
}
int TiRtcWhipConnect(const char *peer, const char *token, TIRTCCONNECTCALLBACK callback, void *user)
{ ++s_whip; return connect_mock(peer, token, callback, user); }
int TiRtcConnect(const char *peer, const char *token, TIRTCCONNECTCALLBACK callback, void *user)
{ ++s_p2p; return connect_mock(peer, token, callback, user); }
int TiRtcConnSetUserData(tirtc_conn_t connection, void *user)
{ connection->user = user; return 0; }
void *TiRtcConnGetUserData(tirtc_conn_t connection) { assert(!connection->closed); return connection->user; }
int TiRtcDisconnect(tirtc_conn_t connection)
{
    assert(!connection->closed);
    ++s_disconnects;
    s_callbacks->on_disconnected(connection);
    connection->closed = true;
    return 0;
}
size_t TiRtcGetSendBufferUsed(tirtc_conn_t connection) { assert(!connection->closed); return s_used; }
int TiRtcSendAudioStream(tirtc_conn_t connection, const TIRTCFRAMEINFO *frame, const void *data)
{
    assert(!connection->closed && frame->stream_id == 1 && frame->media == TIRTC_AUDIO_ALAW);
    assert(frame->flags == TIRTC_AUDIOSAMPLE_8K16B1C && frame->length == 160 && data);
    ++s_frames; return s_send_result;
}
int TiRtcSendCommand(tirtc_conn_t connection, uint32_t command, const void *data, uint32_t length)
{
    assert(!connection->closed && length == 2 && !memcmp(data, "{}", 2));
    s_command_word = command;
    return s_command_result;
}
int TiRtcSubscribeAudio(tirtc_conn_t connection, uint8_t stream)
{ assert(!connection->closed && stream == 1); return s_subscribe_result; }
int TiRtcUnsubscribeAudio(tirtc_conn_t connection, uint8_t stream)
{ assert(!connection->closed && stream == 1); return s_subscribe_result; }
int TiRtcServiceRequest(const char *path, const char *json, const char *token,
                        TIRTCSERVICEREQUESTCALLBACK callback, void *context)
{ (void)path; (void)json; (void)callback; (void)context; assert(!token); return 0; }

static bool receive_audio(uint32_t generation, uint8_t stream, uint32_t pts,
                           const uint8_t *data, size_t length, void *context)
{
    assert(generation == 2 && stream == 1 && data && length == 160 && context == (void *)3);
    assert(pts == s_receive_pts + s_receive_attempts * 20U);
    assert(!memcmp(data, s_receive_expected + s_receive_attempts * 160U, 160));
    if (s_receive_attempts++ == s_receive_capacity) return false;
    if (s_receive_connection_error) {
        s_receive_connection_error = false;
        s_callbacks->on_conn_error(&s_handle2, TIRTC_E_INTERNAL_ERROR);
    }
    ++s_receives; return true;
}

static void receive_packet(const uint8_t *data, size_t length, uint32_t pts, unsigned capacity)
{
    s_receive_expected = data;
    s_receive_pts = pts;
    s_receive_attempts = 0;
    s_receive_capacity = capacity;
    TIRTCFRAMEINFO frame = {.stream_id = 1, .media = TIRTC_AUDIO_ALAW,
                            .flags = TIRTC_AUDIOSAMPLE_8K16B1C, .ts = pts, .length = length};
    s_callbacks->on_audio(&s_handle2, &frame, (void *)data);
}
static xmini_rtc_event_t next(xmini_rtc_event_kind_t kind, uint32_t generation)
{
    xmini_rtc_event_t event;
    assert(xmini_rtc_poll(&event));
    assert(event.kind == kind && event.generation == generation);
    return event;
}

static void test_connect_input_case(size_t peer_length, size_t token_length, unsigned completion)
{
    static uint32_t generation = 100;
    ++generation;
    char *peer = malloc(peer_length + 1), *token = malloc(token_length + 1);
    assert(peer && token);
    for (size_t i = 0; i < peer_length; ++i) peer[i] = (char)('a' + i % 26);
    peer[peer_length] = 0;
    for (size_t i = 0; i < token_length; ++i) token[i] = (char)('A' + i % 26);
    token[token_length] = 0;
    s_expected_peer = peer; s_expected_token = token;
    unsigned submitted = s_whip;
    s_submit_result = completion == 1 ? TIRTC_E_BUSY : 0;
    int result = xmini_rtc_connect(XMINI_RTC_WHIP, generation, peer, token);
    bool valid = peer_length < XMINI_RTC_PEER_CAPACITY && token_length < XMINI_RTC_TOKEN_CAPACITY;
    if (!valid) {
        assert(result == TIRTC_E_INVALID_PARAMETER && s_whip == submitted);
    } else if (completion == 1) {
        assert(result == TIRTC_E_BUSY && s_whip == submitted + 1);
    } else {
        assert(result == 0 && s_whip == submitted + 1);
        assert(s_peer != peer && s_token != token);
        assert(xmini_rtc_connect(XMINI_RTC_WHIP, generation + 1000, peer, token) == TIRTC_E_BUSY);
    }
    s_expected_peer = s_expected_token = NULL;
    memset(peer, '!', peer_length); memset(token, '!', token_length);
    free(peer); free(token);
    s_submit_result = 0;
    if (!valid || completion == 1) return;

    // SDK pointers must remain intact after the borrowed caller inputs are gone.
    assert(strlen(s_peer) == peer_length && strlen(s_token) == token_length);
    for (size_t i = 0; i < peer_length; ++i) assert(s_peer[i] == (char)('a' + i % 26));
    for (size_t i = 0; i < token_length; ++i) assert(s_token[i] == (char)('A' + i % 26));
    s_handle = (struct _tirtc_conn){0};
    if (completion == 0) {
        s_connect(TIRTC_E_TIMEOUTED, NULL, s_user);
        assert(next(XMINI_RTC_CONNECTED, generation).error == TIRTC_E_TIMEOUTED);
    } else if (completion == 2) {
        s_connect(0, &s_handle, s_user);
        next(XMINI_RTC_CONNECTED, generation);
        assert(xmini_rtc_disconnect(generation) == 0);
        next(XMINI_RTC_CLOSED, generation);
    } else {
        assert(completion == 3 && xmini_rtc_cancel(generation) == 0);
        s_connect(0, &s_handle, s_user);
        next(XMINI_RTC_REJECTED, generation);
        xmini_rtc_process();
        next(XMINI_RTC_CLOSED, generation);
    }
    xmini_rtc_process();
    xmini_rtc_status_t status;
    xmini_rtc_status(&status);
    assert(!status.connecting && !status.connected && !status.closing && !status.sdk_pending);
    xmini_rtc_event_t event;
    assert(!xmini_rtc_poll(&event));
}

static void test_connect_input_limits(void)
{
    _Static_assert(XMINI_RTC_PEER_CAPACITY == 2048 && XMINI_RTC_TOKEN_CAPACITY == 1024,
                   "Changing the shared input contract needs new boundary evidence");
    const size_t lengths[] = {128, 129, 149, 2047, 2048};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i)
        test_connect_input_case(lengths[i], 307, 0);
    test_connect_input_case(149, 1023, 0);
    test_connect_input_case(149, 1024, 0);
    test_connect_input_case(149, 307, 1); // Immediate SDK rejection releases both owned inputs.
    test_connect_input_case(149, 307, 2); // Next join succeeds and owns inputs until close.
    test_connect_input_case(149, 307, 3); // Cancel before the delayed successful callback.
    s_handle = (struct _tirtc_conn){0};
    s_handle2 = (struct _tirtc_conn){0};
    s_whip = s_disconnects = 0;
}

static void test_command_metadata(void)
{
    xmini_rtc_command_diagnostic_summary_t before, after;
    xmini_rtc_command_diagnostic_t row;
    xmini_rtc_event_t event;
    xmini_rtc_command_diagnostic(0, &before, NULL);
    void *user = s_handle2.user;
    s_handle2.user = NULL;
    s_callbacks->on_command(&s_handle2, 0x2000, "{}", 2);
    s_handle2.user = user;
    assert(!xmini_rtc_poll(&event)); /* Published connection with observed userdata=0 still drops. */
    assert(xmini_rtc_command_diagnostic(before.captures % 4, &after, &row));
    assert(after.captures == before.captures + 1 && row.observed_generation == 0 && row.current_generation == 2);
    assert(row.connection == (uint32_t)(uintptr_t)&s_handle2 && row.command == 0x2000 && row.length == 2);
    assert((row.flags & (XMINI_RTC_CMD_CONNECTED | XMINI_RTC_CMD_CONNECTION_MATCH)) ==
           (XMINI_RTC_CMD_CONNECTED | XMINI_RTC_CMD_CONNECTION_MATCH));
    assert(!(row.flags & (XMINI_RTC_CMD_ACTIVE | XMINI_RTC_CMD_GENERATION_MATCH | XMINI_RTC_CMD_QUEUED)));
    assert((row.flags & XMINI_RTC_CMD_DISPOSITION_MASK) == (XMINI_RTC_CMD_INACTIVE << XMINI_RTC_CMD_DISPOSITION_SHIFT));
    for (unsigned i = 0; i < 7; ++i) {
        uint32_t word = MAKE_CMDW(0x2000, i);
        s_callbacks->on_command(&s_handle2, word, "{}", 2);
        event = next(XMINI_RTC_COMMAND, 2);
        assert(event.command == word && event.length == 2 && !strcmp(xmini_rtc_event_payload(&event), "{}"));
        xmini_rtc_event_release(&event);
    }
    xmini_rtc_command_diagnostic(0, &after, NULL);
    assert(after.captures == before.captures + 8 && after.overwritten == after.captures - 4);
    for (uint32_t sequence = after.captures - 3; sequence <= after.captures; ++sequence) {
        assert(xmini_rtc_command_diagnostic((sequence - 1) % 4, NULL, &row) && row.sequence == sequence);
        assert((row.flags & XMINI_RTC_CMD_DISPOSITION_MASK) == (XMINI_RTC_CMD_EVENT << XMINI_RTC_CMD_DISPOSITION_SHIFT));
        assert(row.flags & XMINI_RTC_CMD_QUEUED);
    }
    s_inspect_command_entry = true;
    s_callbacks->on_command(&s_handle2, 0x2000, "{}", 2);
    s_inspect_command_entry = false;
    assert(s_command_entries == 1);
    next(XMINI_RTC_COMMAND, 2);
    xmini_rtc_command_tx_diagnostic_t sent, unchanged;
    assert(xmini_rtc_send_command(2, 0x2000, "{}", 2) == 6);
    xmini_rtc_command_tx_diagnostic(&sent);
    assert(sent.sequence && sent.generation == 2 && sent.command == 0x2000 && sent.result == 6 && (sent.flags & 1));
    assert(xmini_rtc_send_command(2, 0x2001, "{}", 2) == 6);
    xmini_rtc_command_tx_diagnostic(&unchanged);
    assert(!memcmp(&sent, &unchanged, sizeof(sent))); /* Last notice is not a fabricated ACK. */
    assert(xmini_rtc_send_command(999, 0x2000, "{}", 2) == TIRTC_E_INVALID_HANDLE);
    xmini_rtc_command_tx_diagnostic(&sent);
    assert(sent.sequence == unchanged.sequence + 1 && sent.generation == 999 && !(sent.flags & 1));
    assert(sent.result == TIRTC_E_INVALID_HANDLE);
    puts("COMMAND metadata: unchanged getgen0 drop, ring coverage, in-flight ENTRY and raw SDK send result PASS");
}

int main(void)
{
    xmini_rtc_transport_diagnostics_t diagnostics;
    tgtrp_internal_diag_t diagnostic = {.version = TGTRP_INTERNAL_DIAG_VERSION,
        .stage = TGTRP_INTERNAL_DIAG_TX, .identity = 0x1234u, .frame_id = 64,
        .frames = 64, .segments = 64, .payload_bytes = 14208, .kind = 1, .stream_id = 10};
    tgtrp_internal_diag_hook(&diagnostic);
    xmini_rtc_transport_diagnostics(&diagnostics);
    assert(!diagnostics.tx.capture_sequence); /* No fabricated sample before SDK init. */
    assert(xmini_rtc_init(receive_audio, (void *)3) == 0);
    assert(xmini_rtc_start("device", "key", "client", "http://sample.invalid") == 0);
    next(XMINI_RTC_STARTED, 0);
    test_connect_input_limits();
    assert(xmini_rtc_uninit() == TIRTC_E_BUSY);
    assert(s_callbacks->on_video == NULL && s_callbacks->on_subscribe_video(&s_handle, 1) < 0);
    char peer[] = "peer", token[] = "token";
    assert(xmini_rtc_connect(XMINI_RTC_WHIP, 1, peer, token) == 0);
    peer[0] = token[0] = '!';
    assert(!strcmp(s_peer, "peer") && !strcmp(s_token, "token"));
    assert(xmini_rtc_cancel(1) == 0);
    s_connect(0, &s_handle, s_user);
    next(XMINI_RTC_REJECTED, 1);
    assert(xmini_rtc_enqueue_audio(1, 1, s_pcma, 160, 0) < 0);
    xmini_rtc_process();
    next(XMINI_RTC_CLOSED, 1);
    assert(s_disconnects == 1);
    xmini_rtc_process();
    assert(xmini_rtc_connect(XMINI_RTC_P2P, 2, "peer", "token") == 0);
    s_connect(0, &s_handle2, s_user);
    next(XMINI_RTC_CONNECTED, 2);
    test_command_metadata();
    for (unsigned stage = TGTRP_INTERNAL_DIAG_TX; stage <= TGTRP_INTERNAL_DIAG_QUEUE; ++stage) {
        diagnostic.stage = (uint8_t)stage;
        diagnostic.stream_id = (uint8_t)(9 + stage);
        diagnostic.rx_allocation_failures = stage == TGTRP_INTERNAL_DIAG_RX ? 7 : 0;
        diagnostic.first_failed_frame_id = stage == TGTRP_INTERNAL_DIAG_RX ? 63 : 0;
        tgtrp_internal_diag_hook(&diagnostic);
    }
    xmini_rtc_transport_diagnostics(&diagnostics);
    assert(diagnostics.tx.capture_sequence == 1 && diagnostics.rx.capture_sequence == 2 &&
           diagnostics.queue.capture_sequence == 3);
    assert(diagnostics.tx.generation == 2 && diagnostics.tx.connection == (uintptr_t)&s_handle2);
    assert(diagnostics.tx.transport.identity == 0x1234u &&
           diagnostics.tx.transport.frames == 64 && diagnostics.tx.transport.payload_bytes == 14208);
    assert(diagnostics.tx.transport.stream_id == 10 && diagnostics.rx.transport.stream_id == 11 &&
           diagnostics.queue.transport.stream_id == 12);
    assert(diagnostics.rx.transport.rx_allocation_failures == 7 &&
           diagnostics.rx.transport.first_failed_frame_id == 63 &&
           !diagnostics.tx.transport.rx_allocation_failures);
    diagnostic.frames = 999; /* Consumer owns copies, not this borrowed stack object. */
    diagnostic.version = 255;
    tgtrp_internal_diag_hook(&diagnostic);
    diagnostic.version = TGTRP_INTERNAL_DIAG_VERSION;
    diagnostic.stage = 255;
    tgtrp_internal_diag_hook(&diagnostic);
    tgtrp_internal_diag_hook(NULL);
    xmini_rtc_transport_diagnostics(NULL);
    xmini_rtc_transport_diagnostics(&diagnostics);
    assert(diagnostics.queue.capture_sequence == 3 && diagnostics.queue.transport.frames == 64);
    assert(xmini_rtc_subscribe(2, 1, true) == 0); /* Real SDK sends five wire bytes. */
    assert(xmini_rtc_subscribe(2, 1, false) == 0);
    s_subscribe_result = TIRTC_E_BUSY;
    assert(xmini_rtc_subscribe(2, 1, true) == TIRTC_E_BUSY);
    assert(xmini_rtc_subscribe(2, 1, false) == TIRTC_E_BUSY);
    s_subscribe_result = 0;
    assert(xmini_rtc_subscribe(2, 1, true) == 0);
    s_subscribe_result = 5;
    assert(xmini_rtc_enqueue_audio(2, 1, s_pcma, 160, 0) < 0);
    assert(xmini_rtc_authorize_audio(1, 1, true) == TIRTC_E_INVALID_HANDLE);
    assert(xmini_rtc_authorize_audio(2, 16, true) == TIRTC_E_INVALID_PARAMETER);
    assert(xmini_rtc_authorize_audio(2, 1, true) == 0);
    xmini_rtc_status_t permission;
    xmini_rtc_status(&permission);
    assert(!permission.subscribed && permission.business_permit == (1U << 1));
    assert(xmini_rtc_enqueue_audio(2, 1, s_pcma, 160, 0) == 0);
    assert(xmini_rtc_authorize_audio(2, 1, false) == 0);
    assert(xmini_rtc_enqueue_audio(2, 1, s_pcma, 160, 0) == TIRTC_E_INVALID_HANDLE);
    assert(xmini_rtc_authorize_audio(2, 1, true) == 0);
    xmini_rtc_process();
    assert(s_frames == 0); /* Regrant cannot revive a packet queued before revoke. */
    assert(xmini_rtc_enqueue_audio(2, 1, s_pcma, 160, 0) == 0);
    s_callbacks->on_unsubscribe_audio(&s_handle2, 1);
    next(XMINI_RTC_UNSUBSCRIBE, 2);
    xmini_rtc_status(&permission);
    assert(!permission.subscribed && !permission.business_permit);
    assert(xmini_rtc_enqueue_audio(2, 1, s_pcma, 160, 0) == TIRTC_E_INVALID_HANDLE);
    assert(xmini_rtc_authorize_audio(2, 1, true) == 0);
    xmini_rtc_process();
    assert(s_frames == 0); /* Remote unsubscribe invalidates business-granted old packets. */
    assert(xmini_rtc_authorize_audio(2, 1, false) == 0);
    assert(s_callbacks->on_subscribe_audio(&s_handle2, 1) == 0);
    next(XMINI_RTC_SUBSCRIBE, 2);
    for (unsigned i = 0; i < 5; ++i) assert(xmini_rtc_enqueue_audio(2, 1, s_pcma, 160, i * 20) == 0);
    assert(xmini_rtc_enqueue_audio(2, 1, s_pcma, 160, 100) == TIRTC_E_BUSY);
    xmini_rtc_process();
    assert(s_frames == 5);
    assert(xmini_rtc_enqueue_audio(2, 1, s_pcma, 160, 100) == 0);
    s_now = 100000;
    xmini_rtc_process();
    assert(s_frames == 5); /* A blocked control operation cannot send stale speech. */
    assert(xmini_rtc_enqueue_audio(2, 1, s_pcma, 160, 120) == 0);
    s_callbacks->on_unsubscribe_audio(&s_handle2, 1);
    next(XMINI_RTC_UNSUBSCRIBE, 2);
    assert(s_callbacks->on_subscribe_audio(&s_handle2, 1) == 0);
    next(XMINI_RTC_SUBSCRIBE, 2);
    xmini_rtc_process();
    assert(s_frames == 5); /* New subscription cannot revive the previous packet. */
    /* Actual H5 failure: two legal 10ms PCMA packets form one playback block. */
    xmini_rtc_status_t actual_80_before, actual_80_after;
    xmini_rtc_status(&actual_80_before);
    TIRTCFRAMEINFO short_frame = {.stream_id = 1, .media = TIRTC_AUDIO_ALAW,
        .flags = TIRTC_AUDIOSAMPLE_8K16B1C, .ts = 20, .length = 80};
    s_callbacks->on_audio(&s_handle2, &short_frame, s_pcma);
    xmini_rtc_status(&actual_80_after);
    assert(actual_80_after.format_errors == actual_80_before.format_errors);
    assert(s_receives == 0);
    short_frame.ts = 30;
    s_callbacks->on_audio(&s_handle2, &short_frame, s_pcma + 80);
    assert(s_receives == 1 && s_receive_attempts == 1);
    s_receives = s_receive_attempts = 0; /* Receiver instrumentation, not SDK counters. */
    TIRTCFRAMEINFO frame = {.stream_id = 1, .media = TIRTC_AUDIO_ALAW,
                            .flags = TIRTC_AUDIOSAMPLE_8K16B1C, .ts = 20, .length = 160};
    s_callbacks->on_audio(&s_handle2, &frame, s_pcma);
    assert(s_receives == 1);
    uint8_t aggregate[160 * 10];
    for (unsigned i = 0; i < sizeof(aggregate); ++i) aggregate[i] = (uint8_t)(i * 17U + i / 160U);
    receive_packet(aggregate, 320, 40, 32); /* Actual AI downlink: both halves, not only the first 160B. */
    assert(s_receives == 3 && s_receive_attempts == 2);
    receive_packet(aggregate, 800, UINT32_MAX - 19U, 32);
    assert(s_receives == 8 && s_receive_attempts == 5); /* PTS wraps naturally per 20ms block. */
    receive_packet(aggregate, 1280, 200, 32);
    assert(s_receives == 16 && s_receive_attempts == 8);
    xmini_rtc_status_t rx_before, rx_after;
    xmini_rtc_status(&rx_before);
    receive_packet(aggregate, sizeof(aggregate), 400, 32);
    xmini_rtc_status(&rx_after);
    assert(s_receives == 24 && s_receive_attempts == 8);
    assert(rx_after.rx_dropped == rx_before.rx_dropped + 2);
    assert(rx_after.format_errors == rx_before.format_errors); /* Callback budget is not a codec error. */
    xmini_rtc_status(&rx_before);
    receive_packet(aggregate, 800, 600, 2);
    xmini_rtc_status(&rx_after);
    assert(s_receives == 26 && s_receive_attempts == 3);
    assert(rx_after.rx_dropped == rx_before.rx_dropped + 3); /* Receiver pressure accounts for all tail blocks. */
    assert(rx_after.format_errors == rx_before.format_errors);
    s_receive_expected = s_pcma;
    s_receive_pts = 20;
    s_receive_attempts = 0;
    s_receive_capacity = 32;
    frame.media = TIRTC_AUDIO_OPUS;
    s_callbacks->on_audio(&s_handle2, &frame, s_pcma);
    next(XMINI_RTC_FORMAT_ERROR, 2);
    assert(s_receives == 26);
    assert(s_format_logs == 1 && strstr(s_format_log, "gen=2 frame=1 data=1"));
    assert(strstr(s_format_log, "stream=1 bytes=160"));
    assert(s_enqueue_log[0]);
    frame.media = TIRTC_AUDIO_ALAW;
    /* Nonempty arbitrary byte counts are valid PCMA. A borrowed SDK payload's
     * declared length must describe its actual readable storage. */
    const uint32_t malformed_lengths[] = {0};
    for (unsigned i = 0; i < sizeof(malformed_lengths) / sizeof(malformed_lengths[0]); ++i) {
        frame.length = malformed_lengths[i];
        s_callbacks->on_audio(&s_handle2, &frame, s_pcma);
    }
    frame.length = 160;
    frame.flags = TIRTC_AUDIOSAMPLE_16K16B1C;
    s_callbacks->on_audio(&s_handle2, &frame, s_pcma);
    frame.flags = TIRTC_AUDIOSAMPLE_8K16B1C;
    frame.stream_id = 16;
    s_callbacks->on_audio(&s_handle2, &frame, s_pcma);
    frame.stream_id = 1;
    s_callbacks->on_audio(&s_handle2, NULL, s_pcma);
    s_callbacks->on_audio(&s_handle2, &frame, NULL);
    assert(s_receives == 26 && s_format_logs == 1); /* No malformed frame reaches the receiver. */
    xmini_rtc_status(&rx_before);
    s_handle2.user = (void *)1;
    receive_packet(aggregate, 320, 700, 32);
    s_handle2.user = (void *)2;
    xmini_rtc_status(&rx_after);
    assert(rx_after.stale_frames == rx_before.stale_frames + 1 && s_receives == 26);
    assert(rx_after.format_errors == rx_before.format_errors);
    const uint32_t bare_commands[] = {0x2000, 0x2001, 0x2100, 0x2200};
    for (unsigned i = 0; i < 4; ++i) {
        assert(xmini_rtc_send_command(2, bare_commands[i], "{}", 2) == 6);
        assert(s_command_word == bare_commands[i]);
    }
    uint32_t request_word = MAKE_CMDW(0x2100, 0xabcd);
    assert(xmini_rtc_send_command(2, request_word, "{}", 2) == 6);
    assert(s_command_word == request_word);
    assert(xmini_rtc_send_command(2, request_word | RESPONSE_BIT, "{}", 2) == 6);
    assert(s_command_word == (request_word | RESPONSE_BIT));
    s_command_result = TIRTC_E_BUSY;
    assert(xmini_rtc_send_command(2, 0x2100, "{}", 2) == TIRTC_E_BUSY);
    s_command_result = 6;
    char message[] = "hello";
    s_callbacks->on_command(&s_handle2, request_word, message, 5);
    message[0] = '!';
    xmini_rtc_event_t event = next(XMINI_RTC_COMMAND, 2);
    assert(event.length == 5 && !strcmp(event.data, "hello"));
    assert(event.command == request_word && event.command_id == 0x2100 &&
           event.command_sequence == 0xabcd && !event.command_response);
    s_callbacks->on_command(&s_handle2, request_word | RESPONSE_BIT, "{}", 2);
    event = next(XMINI_RTC_COMMAND, 2);
    assert(event.command == (request_word | RESPONSE_BIT) && event.command_id == 0x2100 &&
           event.command_sequence == 0xabcd && event.command_response);
    for (unsigned i = 0; i < 4; ++i) {
        s_callbacks->on_command(&s_handle2, bare_commands[i], "{}", 2);
        event = next(XMINI_RTC_COMMAND, 2);
        assert(event.command == bare_commands[i]); /* Bare 0x2001 remains hangup. */
        uint32_t word = MAKE_CMDW(bare_commands[i], i ? 0xffff : 0);
        s_callbacks->on_command(&s_handle2, word, "{}", 2);
        event = next(XMINI_RTC_COMMAND, 2);
        assert(event.command == word && event.command_id == bare_commands[i] &&
               event.command_sequence == (i ? 0xffff : 0));
    }
    TIRTCFRAMEINFO message_frame = {.stream_id = 7, .length = 2};
    s_callbacks->on_message(&s_handle2, &message_frame, "{}");
    event = next(XMINI_RTC_MESSAGE, 2);
    assert(event.command == 7 && !event.command_id && !event.command_sequence && !event.command_response);
    char long_command[9001]; memset(long_command, 'x', 9000); long_command[9000] = 0;
    s_callbacks->on_command(&s_handle2, MAKE_CMDW(0x2200, 5), long_command, 9000);
    s_callbacks->on_command(&s_handle2, MAKE_CMDW(0x2200, 6), long_command, 9000);
    event = next(XMINI_RTC_COMMAND, 2);
    assert(event.length == 9000 && event.payload && xmini_rtc_event_payload(&event)[0] == 'x');
    long_command[0] = '!';
    assert(xmini_rtc_event_payload(&event)[0] == 'x');
    xmini_rtc_status_t payload_status;
    xmini_rtc_status(&payload_status); assert(payload_status.command_bytes_pending == 9001);
    xmini_rtc_event_release(&event); xmini_rtc_event_release(&event);
    xmini_rtc_status(&payload_status); assert(payload_status.command_bytes_pending == 0);
    event = next(XMINI_RTC_ERROR, 2);
    assert(event.error == TIRTC_E_LACK_OF_RESOURCE && event.command_id == 0x2200 &&
           event.command == MAKE_CMDW(0x2200, 6) && event.command_sequence == 6);
    s_callbacks->on_unsubscribe_audio(&s_handle2, 1);
    next(XMINI_RTC_UNSUBSCRIBE, 2);
    assert(xmini_rtc_enqueue_audio(2, 1, s_pcma, 160, 20) < 0);
    assert(xmini_rtc_disconnect(1) == TIRTC_E_INVALID_HANDLE);
    xmini_rtc_status(&rx_before);
    s_receive_connection_error = true;
    receive_packet(aggregate, 800, 800, 32);
    xmini_rtc_status(&rx_after);
    assert(s_receive_attempts == 1 && s_receives == 27);
    assert(rx_after.stale_frames == rx_before.stale_frames + 4);
    assert(rx_after.rx_dropped == rx_before.rx_dropped);
    next(XMINI_RTC_ERROR, 2);
    assert(xmini_rtc_disconnect(2) == 0);
    next(XMINI_RTC_CLOSED, 2);
    assert(xmini_rtc_send_command(2, 0x2100, "{}", 2) == TIRTC_E_INVALID_HANDLE);
    xmini_rtc_process();
    s_handle = (struct _tirtc_conn){0};
    assert(xmini_rtc_arm_inbound(3, true) == 0);
    s_callbacks->on_conn_accepted(&s_handle);
    next(XMINI_RTC_ACCEPTED, 3);
    assert(s_callbacks->on_subscribe_audio(&s_handle, 1) == 0);
    next(XMINI_RTC_SUBSCRIBE, 3);
    assert(xmini_rtc_enqueue_audio(3, 1, s_pcma, 160, 20) == 0);
    s_send_result = TIRTC_E_INTERNAL_ERROR;
    xmini_rtc_process();
    event = next(XMINI_RTC_ERROR, 3);
    assert(event.error == TIRTC_E_INTERNAL_ERROR);
    assert(xmini_rtc_enqueue_audio(3, 1, s_pcma, 160, 40) == TIRTC_E_INVALID_HANDLE);
    s_send_result = 172;
    assert(xmini_rtc_cancel(3) == 0);
    next(XMINI_RTC_CLOSED, 3);
    s_handle2 = (struct _tirtc_conn){0};
    s_callbacks->on_conn_accepted(&s_handle2);
    next(XMINI_RTC_REJECTED, 0);
    xmini_rtc_process();
    next(XMINI_RTC_CLOSED, 0);
    xmini_rtc_process();
    s_submit_result = TIRTC_E_BUSY;
    assert(xmini_rtc_connect(XMINI_RTC_WHIP, 4, "peer", "token") == TIRTC_E_BUSY);
    s_submit_result = 0;
    assert(xmini_rtc_connect(XMINI_RTC_WHIP, 5, "peer", "token") == 0);
    s_connect(TIRTC_E_TIMEOUTED, NULL, s_user);
    event = next(XMINI_RTC_CONNECTED, 5);
#ifdef TIRTC_PENDING_CLOSE_CONTRACT
    assert(event.retired && event.error == TIRTC_E_TIMEOUTED);
    xmini_rtc_process();
    s_sdk_pending = 1;
    assert(xmini_rtc_cancel(6) == 0);
    xmini_rtc_process();
    xmini_rtc_status_t pending_status;
    xmini_rtc_status(&pending_status);
    assert(pending_status.closing && pending_status.sdk_pending == 1);
    assert(!xmini_rtc_poll(&event));
    assert(xmini_rtc_connect(XMINI_RTC_WHIP, 7, "peer", "token") == TIRTC_E_BUSY);
    s_sdk_pending = 0;
    xmini_rtc_process();
    event = next(XMINI_RTC_RETIRED, 6);
    assert(event.retired);
    xmini_rtc_status(&pending_status);
    assert(!pending_status.closing && !pending_status.sdk_pending);
#else
    assert(!event.retired);
#endif
    xmini_rtc_process();
    for (unsigned i = 0; i < 9; ++i) s_callbacks->on_event(TIRTC_EVENT_ACCESS_HIJACKING, NULL, 0);
    next(XMINI_RTC_OVERFLOW, 0);
    xmini_rtc_process();
    xmini_rtc_status_t status;
    xmini_rtc_status(&status);
    assert(status.faulted && status.event_overflow >= 1 && !status.online);
    assert(status.tx_frames == 5 && status.rx_frames == s_receives + 1 && status.format_errors == 6);
    bool got_stopped = false;
    while (xmini_rtc_poll(&event)) if (event.kind == XMINI_RTC_STOPPED) got_stopped = true;
    assert(got_stopped); /* Overflow cannot strand a completed Stop. */
    assert(xmini_rtc_uninit() == 0 && s_uninit == 1);
    xmini_rtc_transport_diagnostics(&diagnostics);
    assert(diagnostics.tx.generation == 2 && diagnostics.tx.transport.frames == 64);
    assert(diagnostics.rx.transport.rx_allocation_failures == 7 &&
           diagnostics.rx.transport.first_failed_frame_id == 63);
    assert(status.transport_capture_sequence == 3); /* Cleanup retains truthful last evidence. */
    assert(s_whip == 3 && s_p2p == 1);
    puts("RTC contract: lifecycle, pending cancel, inbound generation, PCMA, queue bounds, stale handle, overflow PASS");
    return 0;
}
