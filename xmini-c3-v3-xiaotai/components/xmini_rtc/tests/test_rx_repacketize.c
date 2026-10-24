/* Reuse the established SDK/FreeRTOS mocks without executing its contract main. */
#define main rtc_legacy_fixture_main
#define TiRtcSubscribeAudio fixture_subscribe_audio
#define TiRtcUnsubscribeAudio fixture_unsubscribe_audio
#include "test_rtc.c"
#undef TiRtcUnsubscribeAudio
#undef TiRtcSubscribeAudio
#undef main

static uint8_t rx_input[160 * 20], rx_output[160 * 32];
static uint32_t rx_output_pts[32], rx_generation = 200;
static unsigned rx_blocks, rx_attempts, rx_capacity = 32;
static uint8_t rx_stream = 11;
static bool rx_connected, rx_watch_allocations;
static unsigned rx_allocations, rx_network_requests;
static bool rx_retire_on_subscribe;
static void (*rx_reenter)(void);

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);
void *__wrap_malloc(size_t size)
{
    if (rx_watch_allocations) ++rx_allocations;
    return __real_malloc(size);
}
void *__wrap_calloc(size_t count, size_t size)
{
    if (rx_watch_allocations) ++rx_allocations;
    return __real_calloc(count, size);
}
void *__wrap_realloc(void *pointer, size_t size)
{
    if (rx_watch_allocations) ++rx_allocations;
    return __real_realloc(pointer, size);
}

int TiRtcSubscribeAudio(tirtc_conn_t connection, uint8_t stream)
{
    assert(!connection->closed && stream < 16);
    ++rx_network_requests;
    if (rx_retire_on_subscribe) {
        rx_retire_on_subscribe = false;
        s_callbacks->on_disconnected(connection);
        connection->closed = true;
    }
    return s_subscribe_result;
}
int TiRtcUnsubscribeAudio(tirtc_conn_t connection, uint8_t stream)
{ return TiRtcSubscribeAudio(connection, stream); }

static bool rx_receive(uint32_t generation, uint8_t stream, uint32_t pts,
                       const uint8_t *data, size_t length, void *context)
{
    assert(generation == rx_generation && stream == rx_stream && context == (void *)0x5258);
    assert(data && length == 160 && rx_attempts < 32);
    if (rx_attempts++ == rx_capacity) return false;
    assert(rx_blocks < 32);
    memcpy(rx_output + rx_blocks * 160U, data, 160);
    rx_output_pts[rx_blocks++] = pts;
    if (rx_reenter) {
        void (*action)(void) = rx_reenter;
        rx_reenter = NULL;
        action();
    }
    return true;
}

static void rx_drain(void)
{
    xmini_rtc_event_t event;
    while (xmini_rtc_poll(&event)) xmini_rtc_event_release(&event);
}

static void rx_clear_observations(void)
{
    rx_blocks = rx_attempts = 0;
    rx_capacity = 32;
    memset(rx_output, 0, sizeof(rx_output));
    memset(rx_output_pts, 0, sizeof(rx_output_pts));
}

static void rx_close(void)
{
    if (rx_connected) {
        assert(xmini_rtc_disconnect(rx_generation) == 0);
        rx_connected = false;
        rx_drain();
        xmini_rtc_process();
        rx_drain();
    }
}

static void rx_open_connection(xmini_rtc_mode_t mode, bool subscribe)
{
    rx_close();
    ++rx_generation;
    rx_clear_observations();
    s_handle2 = (struct _tirtc_conn){0};
    assert(xmini_rtc_connect(mode, rx_generation, "peer", "token") == 0);
    s_connect(0, &s_handle2, s_user);
    next(XMINI_RTC_CONNECTED, rx_generation);
    rx_connected = true;
    if (subscribe) assert(xmini_rtc_subscribe(rx_generation, rx_stream, true) == 0);
}

static void rx_open(void)
{ rx_open_connection(XMINI_RTC_P2P, true); }

static void rx_feed_stream(const uint8_t *data, size_t length, uint32_t pts, uint8_t stream)
{
    TIRTCFRAMEINFO frame = {.stream_id = stream, .media = TIRTC_AUDIO_ALAW,
        .flags = TIRTC_AUDIOSAMPLE_8K16B1C, .ts = pts, .length = (uint32_t)length};
    bool previous_watch = rx_watch_allocations;
    rx_watch_allocations = true;
    s_callbacks->on_audio(&s_handle2, &frame, (void *)data);
    rx_watch_allocations = previous_watch;
    assert(rx_allocations == 0);
}

static void rx_feed(const uint8_t *data, size_t length, uint32_t pts)
{ rx_feed_stream(data, length, pts, rx_stream); }

static void rx_expect(const uint8_t *data, unsigned blocks, uint32_t first_pts)
{
    assert(rx_blocks == blocks);
    assert(!memcmp(rx_output, data, blocks * 160U));
    for (unsigned i = 0; i < blocks; ++i) assert(rx_output_pts[i] == first_pts + i * 20U);
}

static int rx_select(uint32_t generation, uint8_t stream, bool selected)
{
    rx_watch_allocations = true;
    int result = xmini_rtc_select_audio(generation, stream, selected);
    rx_watch_allocations = false;
    assert(!rx_allocations);
    return result;
}

static void test_local_whip_rx_selection(void)
{
    rx_stream = 0;
    rx_open_connection(XMINI_RTC_WHIP, false);
    unsigned requests = rx_network_requests;
    xmini_rtc_status_t before, after;
    xmini_rtc_status(&before);
    rx_feed(rx_input, 80, 1000);
    xmini_rtc_status(&after);
    assert(!rx_blocks && !after.rx_carry_bytes);
    assert(after.rx_unselected_frames == before.rx_unselected_frames + 1);
    assert(rx_select(rx_generation, 0, true) == 0);
    rx_feed(rx_input, 80, 1000);
    rx_feed(rx_input + 80, 80, 1010);
    assert(rx_blocks == 1 && "LOCAL_WX_RX_REQUIRED_80_PLUS_80");
    rx_expect(rx_input, 1, 1000);
    xmini_rtc_status(&after);
    assert(rx_network_requests == requests && !after.subscribed && !after.business_permit);
    assert(after.rx_input_generation == rx_generation && after.rx_input_stream == 0);

    rx_feed(rx_input + 160, 80, 1020);
    assert(rx_select(rx_generation, 0, true) == 0);
    rx_feed_stream(rx_input + 240, 80, 1030, 1);
    xmini_rtc_status(&after);
    assert(after.rx_carry_bytes == 80);
    assert(rx_select(rx_generation - 1, 0, false) == TIRTC_E_INVALID_HANDLE);
    s_task = (void *)2;
    assert(rx_select(rx_generation, 0, false) == TIRTC_E_INVALID_PARAMETER);
    s_task = (void *)1;
    assert(rx_select(0, 0, true) == TIRTC_E_INVALID_PARAMETER);
    assert(rx_select(rx_generation, 16, true) == TIRTC_E_INVALID_PARAMETER);
    assert(rx_select(rx_generation, 1, false) == 0);
    rx_feed(rx_input + 240, 80, 1030);
    rx_expect(rx_input, 2, 1000);
    assert(rx_network_requests == requests);

    rx_feed(rx_input + 320, 80, 1040);
    s_subscribe_result = TIRTC_E_BUSY;
    assert(xmini_rtc_subscribe(rx_generation, 1, true) == TIRTC_E_BUSY);
    xmini_rtc_status(&before);
    assert(before.rx_carry_bytes == 80);
    s_subscribe_result = 5;
    assert(xmini_rtc_subscribe(rx_generation, 1, true) == 0);
    xmini_rtc_status(&after);
    assert(!after.rx_carry_bytes && after.rx_partial_dropped == before.rx_partial_dropped + 80);
    rx_stream = 1;
    rx_clear_observations();
    rx_feed(rx_input + 320, 80, 2000);
    rx_feed(rx_input + 400, 80, 2010);
    rx_expect(rx_input + 320, 1, 2000);
    assert(rx_network_requests == requests + 2 && !after.subscribed && !after.business_permit);
    assert(rx_select(rx_generation, 1, false) == 0);
    rx_feed(rx_input, 80, 2020);
    xmini_rtc_status(&after);
    assert(!after.rx_carry_bytes);

    assert(rx_select(rx_generation, 1, true) == 0);
    rx_feed(rx_input, 80, 3000);
    assert(xmini_rtc_cancel(rx_generation) == 0);
    rx_connected = false;
    rx_drain(); xmini_rtc_process(); rx_drain();
    xmini_rtc_status(&after);
    assert(!after.connected && !after.closing && !after.sdk_pending && !after.rx_carry_bytes);
    assert(rx_select(rx_generation, 1, true) == TIRTC_E_INVALID_HANDLE);

    rx_stream = 0;
    rx_open_connection(XMINI_RTC_WHIP, false);
    rx_feed(rx_input, 80, 4000);
    assert(!rx_blocks);
    assert(rx_select(rx_generation - 1, 0, true) == TIRTC_E_INVALID_HANDLE);
    assert(rx_select(rx_generation, 0, true) == 0);
    rx_feed(rx_input, 80, 4000); rx_feed(rx_input + 80, 80, 4010);
    rx_expect(rx_input, 1, 4000);
    rx_retire_on_subscribe = true;
    assert(xmini_rtc_subscribe(rx_generation, 0, true) == TIRTC_E_INVALID_HANDLE);
    rx_connected = false;
    rx_drain(); xmini_rtc_process(); rx_drain();
    assert(rx_select(rx_generation, 0, true) == TIRTC_E_INVALID_HANDLE);
    rx_stream = 11;
}

static void test_byte_splits(void)
{
    rx_open();
    xmini_rtc_status_t before, after;
    xmini_rtc_status(&before);
    rx_feed(rx_input, 80, 1000);
    assert(!rx_blocks);
    rx_feed(rx_input + 80, 80, 1010);
    rx_expect(rx_input, 1, 1000);
    xmini_rtc_status(&after);
    assert(after.format_errors == before.format_errors && after.rx_frames == before.rx_frames + 1);

    rx_open();
    rx_feed(rx_input, 80, 1000);
    rx_feed(rx_input + 80, 80, 9000); /* Web timestamps are batch wall time, not a gap signal. */
    rx_expect(rx_input, 1, 1000);

    rx_open();
    rx_feed(rx_input, 1, 1000);
    rx_feed(rx_input + 1, 159, 1000);
    rx_expect(rx_input, 1, 1000);

    rx_open();
    rx_feed(rx_input, 341, 1000);
    assert(rx_blocks == 2);
    rx_feed(rx_input + 341, 299, 1042);
    rx_expect(rx_input, 4, 1000);

    rx_open();
    rx_feed(rx_input, 640, 1000);
    rx_feed(rx_input + 640, 1, 1000);
    rx_feed(rx_input + 641, 159, 9000);
    assert(rx_blocks == 5 && !memcmp(rx_output, rx_input, 800));
    for (unsigned i = 0; i < 4; ++i) assert(rx_output_pts[i] == 1000 + i * 20U);
    assert(rx_output_pts[4] == 1000); /* A separate batch can have the same wall-clock stamp. */

    rx_open();
    for (unsigned i = 0; i < 160; ++i) rx_feed(rx_input + i, 1, 1000 + i / 8U);
    rx_expect(rx_input, 1, 1000);

    rx_open();
    rx_feed(rx_input, 341, UINT32_MAX - 19U);
    rx_feed(rx_input + 341, 299, 22);
    rx_expect(rx_input, 4, UINT32_MAX - 19U);
}

static void test_generation_and_subscription_boundaries(void)
{
    rx_open();
    rx_feed(rx_input, 80, 1000);
    rx_open();
    rx_feed(rx_input + 160, 80, 2000);
    rx_feed(rx_input + 240, 80, 2010);
    rx_expect(rx_input + 160, 1, 2000);

    rx_open();
    rx_feed(rx_input, 80, 1000);
    struct _tirtc_conn obsolete = {.user = (void *)(uintptr_t)(rx_generation - 1U)};
    TIRTCFRAMEINFO stale = {.stream_id = rx_stream, .media = TIRTC_AUDIO_OPUS,
        .flags = TIRTC_AUDIOSAMPLE_8K16B1C, .ts = 9000, .length = 80};
    xmini_rtc_status_t before, after;
    xmini_rtc_status(&before);
    s_callbacks->on_audio(&obsolete, &stale, rx_input + 160);
    xmini_rtc_status(&after);
    assert(after.rx_carry_bytes == 80 && after.format_errors == before.format_errors);
    assert(after.stale_frames == before.stale_frames + 1);
    rx_feed(rx_input + 80, 80, 1010);
    rx_expect(rx_input, 1, 1000);

    rx_open();
    rx_feed(rx_input, 80, 1000);
    assert(xmini_rtc_subscribe(rx_generation, rx_stream, false) == 0);
    rx_feed(rx_input + 80, 80, 1010);
    assert(!rx_blocks);
    assert(xmini_rtc_subscribe(rx_generation, rx_stream, true) == 0);
    rx_feed(rx_input + 160, 80, 2000);
    rx_feed(rx_input + 240, 80, 2010);
    rx_expect(rx_input + 160, 1, 2000);

    rx_open();
    rx_feed(rx_input, 80, 1000);
    assert(xmini_rtc_subscribe(rx_generation, rx_stream, false) == 0);
    rx_stream = 10;
    assert(xmini_rtc_subscribe(rx_generation, rx_stream, true) == 0);
    rx_feed(rx_input + 160, 80, 2000);
    rx_feed_stream(rx_input + 80, 80, 1010, 11);
    rx_feed(rx_input + 240, 80, 2010);
    rx_expect(rx_input + 160, 1, 2000);
    rx_stream = 11;
}

static void rx_revoke_and_regrant(void)
{
    assert(xmini_rtc_subscribe(rx_generation, rx_stream, false) == 0);
    assert(xmini_rtc_subscribe(rx_generation, rx_stream, true) == 0);
    rx_feed(rx_input + 1600, 80, 9000); /* Reentrant input is bounded rejection, not new carry. */
}

static void test_reentrant_subscription_epoch(void)
{
    rx_open();
    xmini_rtc_status_t before, after;
    xmini_rtc_status(&before);
    rx_reenter = rx_revoke_and_regrant;
    rx_feed(rx_input, 640, 1000);
    rx_expect(rx_input, 1, 1000);
    xmini_rtc_status(&after);
    assert(after.rx_frames == before.rx_frames + 1 && after.rx_carry_bytes == 0);
    assert(after.format_errors == before.format_errors && after.rx_bytes_dropped >= before.rx_bytes_dropped + 560);
    assert(after.rx_input_generation == rx_generation && after.rx_input_chunks == 2);
    assert(after.rx_input_valid && after.rx_input_length == 80 && after.rx_input_stream == rx_stream);
    rx_clear_observations();
    rx_feed(rx_input + 1600, 80, 9000);
    rx_feed(rx_input + 1680, 80, 9010);
    rx_expect(rx_input + 1600, 1, 9000);
}

static void test_pressure_and_budget(void)
{
    rx_open();
    xmini_rtc_status_t before, after;
    xmini_rtc_status(&before);
    rx_capacity = 0;
    rx_feed(rx_input, 80, 1000);
    rx_feed(rx_input + 80, 380, 1010);
    assert(!rx_blocks && rx_attempts == 1);
    rx_capacity = 32;
    rx_feed(rx_input + 640, 80, 2000);
    rx_feed(rx_input + 720, 80, 2010);
    rx_expect(rx_input + 640, 1, 2000);
    xmini_rtc_status(&after);
    assert(after.rx_dropped > before.rx_dropped && after.format_errors == before.format_errors);

    rx_open();
    rx_feed(rx_input, 160U * 9U + 80U, 1000);
    rx_expect(rx_input, 8, 1000);
    rx_clear_observations();
    rx_feed(rx_input + 1600, 80, 2000);
    rx_feed(rx_input + 1680, 80, 2010);
    rx_expect(rx_input + 1600, 1, 2000);
}

static void test_oversized_frame_reject_accounting(void)
{
    rx_open();
    rx_feed(rx_input, 159, 1000);
    rx_capacity = 0;
    xmini_rtc_status_t before, after;
    xmini_rtc_status(&before);
    assert(before.rx_carry_bytes == 159);
    const uint8_t prefix[1] = {0x5a};
    /* Fault-injected FI: only the one consumed byte is readable, not a 4 GiB payload. */
    rx_feed(prefix, UINT32_MAX, 1020);
    xmini_rtc_status(&after);
    assert(rx_blocks == 0 && rx_attempts == 1);
    assert(after.rx_dropped == before.rx_dropped + (UINT32_MAX - 1U) / 160U + 1U);
    assert(after.rx_partial_dropped == before.rx_partial_dropped + (UINT32_MAX - 1U) % 160U);
    assert(after.rx_bytes_dropped == before.rx_bytes_dropped + 158U);
    assert(after.rx_carry_bytes == 0 && after.rx_frames == before.rx_frames);
    assert(after.format_errors == before.format_errors && !rx_allocations);
}

static void test_stop_discards_partial(void)
{
    rx_open();
    rx_feed(rx_input, 80, 1000);
    assert(xmini_rtc_stop() == 0);
    rx_connected = false;
    assert(rx_select(rx_generation, rx_stream, true) == TIRTC_E_INVALID_HANDLE);
    rx_feed(rx_input + 80, 80, 1010);
    assert(!rx_blocks);
    rx_drain();
    assert(xmini_rtc_uninit() == 0);
    s_step = 0;
    assert(xmini_rtc_start("device", "key", "client", "http://sample.invalid") == 0);
    next(XMINI_RTC_STARTED, 0);
    rx_open();
    rx_feed(rx_input + 160, 80, 2000);
    rx_feed(rx_input + 240, 80, 2010);
    rx_expect(rx_input + 160, 1, 2000);
}

static void expect_input(const xmini_rtc_status_t *state, uint32_t generation,
                         uint32_t chunks, uint32_t length, uint8_t stream)
{
    assert(state->rx_input_valid == 1 && state->rx_input_generation == generation);
    assert(state->rx_input_chunks == chunks && state->rx_input_length == length);
    assert(state->rx_input_media == TIRTC_AUDIO_ALAW);
    assert(state->rx_input_flags == TIRTC_AUDIOSAMPLE_8K16B1C);
    assert(state->rx_input_stream == stream);
}

static void expect_same_input(const xmini_rtc_status_t *before, const xmini_rtc_status_t *after)
{
    assert(after->rx_input_generation == before->rx_input_generation);
    assert(after->rx_input_chunks == before->rx_input_chunks);
    assert(after->rx_input_length == before->rx_input_length);
    assert(after->rx_input_media == before->rx_input_media);
    assert(after->rx_input_flags == before->rx_input_flags);
    assert(after->rx_input_stream == before->rx_input_stream);
    assert(after->rx_input_valid == before->rx_input_valid);
}

static void test_validated_input_snapshot(void)
{
    xmini_rtc_status_t before, after;
    rx_open();
    rx_feed(rx_input, 80, 1000);
    xmini_rtc_status(&before);
    expect_input(&before, rx_generation, 1, 80, 11);
    assert(!rx_blocks); /* Input metadata is independent of a complete playback block. */
    rx_feed(rx_input + 80, 80, 1010);
    xmini_rtc_status(&after);
    expect_input(&after, rx_generation, 2, 80, 11);
    assert(rx_blocks == 1);

    before = after;
    rx_feed_stream(rx_input + 160, 80, 2000, 10);
    xmini_rtc_status(&after);
    expect_same_input(&before, &after);
    assert(after.rx_unselected_frames == before.rx_unselected_frames + 1);

    struct _tirtc_conn obsolete = {.user = (void *)(uintptr_t)(rx_generation - 1U)};
    TIRTCFRAMEINFO frame = {.stream_id = 11, .media = TIRTC_AUDIO_ALAW,
        .flags = TIRTC_AUDIOSAMPLE_8K16B1C, .ts = 2000, .length = 80};
    s_callbacks->on_audio(&obsolete, &frame, rx_input);
    xmini_rtc_status(&after);
    expect_same_input(&before, &after);
    frame.media = TIRTC_AUDIO_OPUS;
    s_callbacks->on_audio(&s_handle2, &frame, rx_input);
    xmini_rtc_status(&after);
    expect_same_input(&before, &after);
    frame.media = TIRTC_AUDIO_ALAW;
    frame.flags = (uint8_t)(TIRTC_AUDIOSAMPLE_8K16B1C + 1U);
    s_callbacks->on_audio(&s_handle2, &frame, rx_input);
    frame.flags = TIRTC_AUDIOSAMPLE_8K16B1C;
    frame.length = 0;
    s_callbacks->on_audio(&s_handle2, &frame, rx_input);
    frame.length = 80;
    s_callbacks->on_audio(&s_handle2, &frame, NULL);
    s_callbacks->on_audio(&s_handle2, NULL, rx_input);
    xmini_rtc_status(&after);
    expect_same_input(&before, &after);
    rx_drain();

    assert(xmini_rtc_subscribe(rx_generation, 11, false) == 0);
    rx_feed(rx_input, 80, 3000);
    xmini_rtc_status(&after);
    expect_same_input(&before, &after);
    assert(xmini_rtc_subscribe(rx_generation, 11, true) == 0);
    rx_feed(rx_input, 341, 4000);
    xmini_rtc_status(&before);
    expect_input(&before, rx_generation, 3, 341, 11);
    rx_close();
    obsolete.user = (void *)(uintptr_t)rx_generation;
    frame.ts = 5000;
    s_callbacks->on_audio(&obsolete, &frame, rx_input);
    xmini_rtc_status(&after);
    expect_same_input(&before, &after);
    rx_open();
    xmini_rtc_status(&after);
    expect_same_input(&before, &after); /* Old generation remains explicitly tagged. */
    rx_feed(rx_input, 80, 6000);
    xmini_rtc_status(&after);
    expect_input(&after, rx_generation, 1, 80, 11);
}

int main(void)
{
    for (unsigned i = 0; i < sizeof(rx_input); ++i) rx_input[i] = (uint8_t)(i * 17U + i / 160U);
    assert(xmini_rtc_init(rx_receive, (void *)0x5258) == 0);
    assert(xmini_rtc_start("device", "key", "client", "http://sample.invalid") == 0);
    next(XMINI_RTC_STARTED, 0);
    xmini_rtc_status_t initial;
    xmini_rtc_status(&initial);
    assert(!initial.rx_input_valid && !initial.rx_input_chunks && !initial.rx_input_generation);
    test_local_whip_rx_selection();
    test_byte_splits();
    test_generation_and_subscription_boundaries();
    test_reentrant_subscription_epoch();
    test_pressure_and_budget();
    test_oversized_frame_reject_accounting();
    test_stop_discards_partial();
    test_validated_input_snapshot();
    rx_close();
    assert(xmini_rtc_stop() == 0);
    rx_drain();
    assert(xmini_rtc_uninit() == 0);
    assert(!rx_allocations);
    puts("RX byte repacketization: split/PTS/ownership/subscription/pressure/stop/no-heap PASS");
    return 0;
}
