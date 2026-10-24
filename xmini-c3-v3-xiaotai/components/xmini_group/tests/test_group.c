// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "../xmini_group.c"

static platform_operation_t last_operation;
static uint32_t last_ticket, last_generation;
static unsigned connect_count, signal_count, profile_count, finish_count;
static int signal_error, connect_error;
static const char *expected_peer, *expected_token;
static char last_body[1024], last_signal[1024];
static char last_log[768];
static unsigned log_count;
void group_test_log(const char *tag, const char *format, ...)
{
    assert(!strcmp(tag, "xmini_group"));
    va_list args;
    va_start(args, format);
    vsnprintf(last_log, sizeof(last_log), format, args);
    va_end(args);
    ++log_count;
}
static int http(platform_operation_t operation, uint32_t ticket, const char *json, void *context)
{ (void)context; assert(ticket); last_operation = operation; last_ticket = ticket; snprintf(last_body, sizeof(last_body), "%s", json ? json : ""); return 0; }
static int connect(uint32_t generation, const char *peer, const char *token, void *context)
{
    (void)context;
    assert(!strcmp(peer, expected_peer ? expected_peer : "peer"));
    assert(!strcmp(token, expected_token ? expected_token : "test-token"));
    last_generation = generation; ++connect_count; return connect_error;
}
static int send_signal(uint32_t generation, uint32_t command, const char *json, void *context)
{ (void)context; assert(generation && command == 0x2200); ++signal_count; snprintf(last_signal, sizeof(last_signal), "%s", json); return signal_error; }
static int profile(uint32_t generation, uint8_t tx, uint8_t rx, void *context)
{ (void)context; assert(generation && tx == 1 && rx == 1); ++profile_count; return 0; }
static void finish(uint32_t generation, int error, void *context)
{ (void)error; (void)context; assert(generation); ++finish_count; }
static uint32_t random_value(void *context) { (void)context; static uint32_t value = 100; return ++value; }

static const char *empty = "{\"code\":200,\"data\":{\"device_id\":\"dev\",\"desired_state\":\"\",\"assignment_version\":0}}";
static const char *assigned = "{\"code\":200,\"data\":{\"device_id\":\"dev\",\"room_id\":\"group_room_exact\",\"room_code\":\"001234\",\"desired_state\":\"joined\",\"assignment_version\":3}}";
static const char *left = "{\"code\":200,\"data\":{\"device_id\":\"dev\",\"desired_state\":\"left\",\"assignment_version\":4}}";
static const char *token = "{\"code\":200,\"data\":{\"peer_id\":\"peer\",\"token\":\"test-token\",\"heartbeat_seconds\":15,\"lease_seconds\":45}}";
static const char *joined = "{\"jsonrpc\":\"2.0\",\"id\":17,\"result\":{\"session_id\":\"server-session\",\"input_audio\":{\"codec\":\"g711a\",\"sample_rate\":8000,\"channels\":1},\"output_audio\":{\"codec\":\"g711a\",\"sample_rate\":8000,\"channels\":1}}}";

static void reset(void)
{
    memset(&s, 0, sizeof(s));
    connect_count = signal_count = profile_count = finish_count = 0;
    signal_error = 0;
    connect_error = 0;
    expected_peer = expected_token = NULL;
    log_count = 0;
    last_log[0] = 0;
    xmini_group_callbacks_t callbacks = {.request = http, .connect = connect, .signal = send_signal,
        .set_profile = profile, .finish = finish, .random = random_value};
    assert(xmini_group_init(&callbacks) == 0);
    assert(xmini_group_init(&callbacks) != 0);
    assert(xmini_group_open(17, "dev", 1000) == 0);
}
static void reply(const char *json, int64_t now) { xmini_group_on_http(last_operation, last_ticket, 0, json, now); }
static void to_connecting(void)
{
    reset();
    assert(xmini_group_action(XMINI_GROUP_CONNECT, 17, NULL, NULL, 1001) == 0);
    reply(assigned, 1100);
    xmini_group_tick(1101);
    assert(last_operation == PLATFORM_GROUP_CONNECT_TOKEN);
    cJSON *body = cJSON_Parse(last_body);
    assert(body && !strcmp(string(body, "room_id", 65), "group_room_exact"));
    assert(strlen(string(body, "session_id", 65)) == 32);
    cJSON_Delete(body);
    reply(token, 1200);
    assert(connect_count == 1 && last_generation == 17 && profile_count == 0);
}
static void to_active(void)
{
    to_connecting();
    xmini_group_on_connected(17, 0, 1300);
    assert(strstr(last_signal, "join_room") && strstr(last_signal, "g711a") && !strstr(last_signal, "opus"));
    xmini_group_on_signal(17, joined, strlen(joined), 1400);
    assert(profile_count == 1 && s.view.active && !s.view.speaking);
}

static void test_rejected_token_metadata(void)
{
    reset();
    assert(xmini_group_action(XMINI_GROUP_CONNECT, 17, NULL, NULL, 1001) == 0);
    reply(assigned, 1100);
    xmini_group_tick(1101);
    reply("{\"code\":200,\"data\":{\"peer_id\":[],\"token\":\"private-test-secret\",\"heartbeat_seconds\":\"15\",\"lease_seconds\":45}}", 1200);
    assert(s.view.error == GROUP_INVALID && !connect_count && finish_count == 1 && log_count == 1);
    assert(strstr(last_log, "peer_type=32 peer_bytes=0 token_type=16 token_bytes=19"));
    assert(strstr(last_log, "hb=0 hb_valid=0 lease=45 lease_valid=0 foreground=1 want_connect=1 closing=0"));
    assert(!strstr(last_log, "private-test-secret"));

    reset();
    assert(xmini_group_action(XMINI_GROUP_CONNECT, 17, NULL, NULL, 1001) == 0);
    reply(assigned, 1100);
    xmini_group_tick(1101);
    cJSON *root = cJSON_Parse(token), *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    char peer[2049];
    memset(peer, 'p', sizeof(peer) - 1); peer[sizeof(peer) - 1] = 0;
    assert(cJSON_ReplaceItemInObjectCaseSensitive(data, "peer_id", cJSON_CreateString(peer)));
    char *json = cJSON_PrintUnformatted(root);
    assert(json);
    reply(json, 1200);
    cJSON_free(json); cJSON_Delete(root);
    assert(s.view.error == GROUP_INVALID && !connect_count && finish_count == 1 && log_count == 1);
    assert(strstr(last_log, "peer_type=16 peer_bytes=2048 token_type=16 token_bytes=10"));
    assert(strstr(last_log, "hb=15 hb_valid=1 lease=45 lease_valid=1"));
    assert(!strstr(last_log, "test-token") && !strstr(last_log, "pppp"));
}

static void prepare_token(void)
{
    reset();
    assert(xmini_group_action(XMINI_GROUP_CONNECT, 17, NULL, NULL, 1001) == 0);
    reply(assigned, 1100);
    xmini_group_tick(1101);
    assert(last_operation == PLATFORM_GROUP_CONNECT_TOKEN);
}

static void reply_token_inputs(const char *peer, const char *credential, int64_t now)
{
    cJSON *root = cJSON_Parse(token), *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    assert(cJSON_ReplaceItemInObjectCaseSensitive(data, "peer_id", cJSON_CreateString(peer)));
    assert(cJSON_ReplaceItemInObjectCaseSensitive(data, "token", cJSON_CreateString(credential)));
    char *json = cJSON_PrintUnformatted(root);
    assert(json);
    reply(json, now);
    memset(json, '!', strlen(json)); cJSON_free(json); cJSON_Delete(root);
}

static void token_with_lengths(size_t peer_length, size_t token_length, bool valid, int error)
{
    char peer[2049], credential[1025];
    assert(peer_length < sizeof(peer) && token_length < sizeof(credential));
    for (size_t i = 0; i < peer_length; ++i) peer[i] = (char)('a' + i % 26);
    peer[peer_length] = 0;
    for (size_t i = 0; i < token_length; ++i) credential[i] = (char)('A' + i % 26);
    credential[token_length] = 0;
    prepare_token();
    expected_peer = peer; expected_token = credential; connect_error = error;
    reply_token_inputs(peer, credential, 1200);
    if (!valid) {
        assert(!connect_count && s.view.error == GROUP_INVALID && finish_count == 1);
        return;
    }
    if (connect_count != 1 || s.view.error != error || finish_count != (unsigned)(error != 0))
        fprintf(stderr, "peer_bytes=%zu token_bytes=%zu connect=%u error=%d finish=%u\n",
                peer_length, token_length, connect_count, s.view.error, finish_count);
    assert(connect_count == 1 && s.view.error == error && finish_count == (unsigned)(error != 0));
    if (!error) {
        assert(s.connecting && s.view.phase == XMINI_GROUP_CONNECTING);
        assert(xmini_group_action(XMINI_GROUP_CONNECT, 17, NULL, NULL, 1201) == GROUP_STATE);
        assert(xmini_group_action(XMINI_GROUP_JOIN, 17, "001234", NULL, 1201) == 40923);
        xmini_group_on_connected(17, -7, 1300);
        assert(s.view.error == -7 && finish_count == 1 && !s.view.active);
        xmini_group_on_closed(17, 0, 1400);
        assert(!s.connecting && !s.closing && s.retired && s.view.assigned);
        assert(s.pending && last_operation == PLATFORM_GROUP_PRESENCE);
        reply("{\"code\":200,\"data\":null}", 1450);
        assert(xmini_group_open(18, "dev", 1500) == 0);
        assert(s.view.generation == 18 && !s.retired);
        assert(xmini_group_action(XMINI_GROUP_CONNECT, 18, NULL, NULL, 1501) == 0);
        reply(assigned, 1600);
        xmini_group_tick(1601);
        assert(last_operation == PLATFORM_GROUP_CONNECT_TOKEN);
        reply_token_inputs(peer, credential, 1700);
        assert(connect_count == 2 && last_generation == 18 && s.connecting);
        xmini_group_suspend(18, 0, 1800);
        xmini_group_on_closed(18, 0, 1900);
        assert(!s.connecting && !s.closing && s.retired && s.view.assigned);
    }
}

static void test_token_connection_limits(void)
{
    const size_t peer_lengths[] = {149, 128, 129, 2047, 2048};
    for (size_t i = 0; i < sizeof(peer_lengths) / sizeof(peer_lengths[0]); ++i)
        token_with_lengths(peer_lengths[i], 307, peer_lengths[i] < 2048, 0);
    const size_t token_lengths[] = {1023, 1024};
    for (size_t i = 0; i < sizeof(token_lengths) / sizeof(token_lengths[0]); ++i)
        token_with_lengths(149, token_lengths[i], token_lengths[i] < 1024, 0);
    token_with_lengths(149, 307, true, -8);
}

int main(void)
{
    test_rejected_token_metadata();
    test_token_connection_limits();
    reset();
    assert(xmini_group_action((xmini_group_action_t)-1, 17, NULL, NULL, 1000) != 0);
    assert(xmini_group_action(XMINI_GROUP_JOIN, 17, "12345", NULL, 1000) != 0);
    assert(xmini_group_action(XMINI_GROUP_JOIN, 17, "001234", "12x4", 1000) != 0);
    uint32_t read_ticket = last_ticket;
    assert(xmini_group_action(XMINI_GROUP_JOIN, 17, "001234", "0123", 1001) == 0);
    reply(empty, 1100);
    assert(last_operation == PLATFORM_GROUP_JOIN && last_ticket != read_ticket);
    assert(strstr(last_body, "001234") && strstr(last_body, "0123"));
    assert(!s.view.assigned); // request acceptance is not membership
    reply(assigned, 1200);
    assert(s.view.assigned && s.view.assignment_version == 3 && !strcmp(s.view.room_code, "001234"));
    assert(xmini_group_action(XMINI_GROUP_CREATE, 17, NULL, NULL, 1201) == 40923);
    xmini_group_on_http(PLATFORM_GROUP_ASSIGNMENT, read_ticket, 0, empty, 1202);
    assert(s.view.assigned && s.view.stale_events == 1);

    to_connecting();
    uint32_t token_ticket = last_ticket;
    xmini_group_suspend(17, 0, 1201);
    assert(s.view.assigned && !s.view.foreground && finish_count == 1);
    xmini_group_on_http(PLATFORM_GROUP_CONNECT_TOKEN, token_ticket, 0, token, 1202);
    xmini_group_on_connected(17, 0, 1203);
    assert(connect_count == 1 && profile_count == 0 && !s.view.active);
    xmini_group_on_closed(17, 0, 1204);
    assert(s.view.assigned && s.view.phase == XMINI_GROUP_ASSIGNED);

    to_connecting();
    xmini_group_on_connected(17, 0, 1300);
    const char *wrong = "{\"jsonrpc\":\"2.0\",\"id\":17,\"result\":{\"session_id\":\"s\",\"input_audio\":{\"codec\":\"alaw\",\"sample_rate\":8000,\"channels\":1},\"output_audio\":{\"codec\":\"g711a\",\"sample_rate\":8000,\"channels\":1}}}";
    xmini_group_on_signal(17, wrong, strlen(wrong), 1400);
    assert(!s.view.active && profile_count == 0 && finish_count == 1 && s.view.assigned);

    to_active();
    xmini_group_tick(1500);
    assert(last_operation == PLATFORM_GROUP_PRESENCE && strstr(last_body, "joined"));
    reply("{\"code\":200,\"data\":null}", 1900);
    assert(s.lease_deadline == 1500 + 45000); // request-time lower bound, not response RTT
    assert(xmini_group_action(XMINI_GROUP_PTT_DOWN, 17, NULL, NULL, 2000) == 0 && s.view.speaking);
    signal_error = -5;
    assert(xmini_group_action(XMINI_GROUP_PTT_UP, 17, NULL, NULL, 2001) == -5 && !s.view.speaking);
    assert(xmini_group_action(XMINI_GROUP_PTT_DOWN, 17, NULL, NULL, 2002) == -5 && !s.view.speaking);
    signal_error = 0;
    const char *snapshot = "{\"jsonrpc\":\"2.0\",\"method\":\"room_snapshot\",\"params\":{\"room_id\":\"app:group_room_exact\",\"self\":{\"participant_id\":\"self\",\"device_id\":\"dev\"},\"participants\":[{\"participant_id\":\"self\",\"device_id\":\"dev\"},{\"participant_id\":\"other\",\"device_id\":\"other-dev\"}]}}";
    xmini_group_on_signal(17, snapshot, strlen(snapshot), 2100);
    assert(s.view.snapshot_ready && s.view.participant_count == 2);
    const char *wrong_room = "{\"jsonrpc\":\"2.0\",\"method\":\"room_closed\",\"params\":{\"room_id\":\"wrong:group_room_exact\"}}";
    xmini_group_on_signal(17, wrong_room, strlen(wrong_room), 2101);
    assert(s.view.active && finish_count == 0);
    xmini_group_on_signal(16, snapshot, strlen(snapshot), 2102);
    assert(s.view.active);
    assert(xmini_group_action(XMINI_GROUP_DISCONNECT, 17, NULL, NULL, 2200) == 0);
    assert(!s.view.active && !s.view.speaking && s.view.assigned && finish_count == 1);
    xmini_group_on_closed(17, 0, 2201);
    reply("{\"code\":200,\"data\":null}", 2202);
    assert(s.view.assigned);
    assert(xmini_group_action(XMINI_GROUP_LEAVE, 17, NULL, NULL, 2300) == 0);
    assert(last_operation == PLATFORM_GROUP_LEAVE && s.view.assigned);
    assert(strstr(last_body, "group_room_exact") && strstr(last_body, "assignment_version"));
    reply(left, 2400);
    assert(!s.view.assigned && s.view.assignment_version == 4);

    to_active();
    xmini_group_tick(s.lease_deadline);
    assert(!s.view.active && finish_count == 1 && s.view.assigned);
    reset();
    reply("{\"code\":0,\"data\":{}}", 1100);
    assert(s.view.error && !s.view.assignment_known);
    reset();
    xmini_group_on_http(last_operation, last_ticket, ESP_FAIL, "{\"code\":40320}", 1100);
    assert(s.view.error == 40320 && !s.view.assignment_known);
    return 0;
}
