// SPDX-License-Identifier: MIT
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"
#include "xiaotai/at_command.h"
#include "xiaotai/port/http.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/sys.h"

static atomic_uint s_quiesce_release;
static atomic_bool s_release_posted, s_show_progress;
static xiaotai_critical_t s_progress_lock;
static xiaotai_net_prepare_t s_last_notice;
static bool s_progress_initialized;
static xiaotai_at_verb_t s_verbs[6];

static void release_owner(void *unused)
{
    (void)unused;
    const uint32_t ticket = atomic_exchange(&s_quiesce_release, 0);
    if (ticket) xiaotai_quiesce_end(ticket);
    atomic_store(&s_release_posted, false);
}

void net_companion_at_work(void)
{
    if (!atomic_load(&s_quiesce_release) || atomic_exchange(&s_release_posted, true)) return;
    if (xiaotai_run(release_owner, NULL) == XIAOTAI_OK) {
        net_deadline_clear(NET_DEADLINE_COPROCESSOR_RELEASE);
    } else {
        atomic_store(&s_release_posted, false);
        net_deadline_set(NET_DEADLINE_COPROCESSOR_RELEASE, xiaotai_time_us() + 10000, net_companion_at_work);
    }
}

static void wake_worker(const net_message_t *message) { (void)message; }

static void release_quiesce(uint32_t ticket)
{
    if (!ticket) return;
    if (xiaotai_on_owner()) {
        xiaotai_quiesce_end(ticket);
    } else {
        atomic_store(&s_quiesce_release, ticket);
        /* A full queue already wakes the worker, which retains the ticket
         * until the actual owner accepts it. */
        (void)net_post(wake_worker, 0, NULL, 0);
    }
}

static xiaotai_err_t quiesce(uint32_t *ticket, const xiaotai_reply_t *reply)
{
    if (xiaotai_activity_current(NULL) || xiaotai_quiesced())
        return xiaotai_at_fail(reply, XIAOTAI_ERR_BUSY, "ACTIVITY");
    return xiaotai_quiesce_begin(XIAOTAI_QUIESCE_COPROCESSOR, ticket);
}

static bool valid_field(const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p <= ' ' || *p >= 127) return false;
    }
    return true;
}

static xiaotai_err_t report_identity(bool live, const xiaotai_reply_t *reply)
{
    xiaotai_coprocessor_info_t info = {0};
    const xiaotai_err_t err = xiaotai_net_coprocessor_info(live, &info);
    if (err != XIAOTAI_OK) return err;
    if (!valid_field(info.project) || !valid_field(info.version) || !valid_field(info.platform) ||
        !valid_field(info.protocol) || !valid_field(info.elf_sha256))
        return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "IDENTITY");
    xiaotai_at_printf(reply, "+COMPANION: project=%s version=%s platform=%s protocol=%s elf_sha256=%s",
                     info.project, info.version, info.platform, info.protocol[0] ? info.protocol : "-", info.elf_sha256);
    return XIAOTAI_OK;
}

static xiaotai_err_t cached(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    return report_identity(false, reply);
}

static bool live_next(void *ctx, const xiaotai_reply_t *reply)
{
    (void)ctx;
    const xiaotai_err_t err = report_identity(true, reply);
    if (err != XIAOTAI_OK) (void)xiaotai_at_fail(reply, err, NULL);
    return false;
}

static xiaotai_err_t live(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    return xiaotai_at_stream(reply, &(xiaotai_at_stream_t){.next = live_next});
}

static xiaotai_err_t retry(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    return net_state_now() == XIAOTAI_NET_BLOCKED ? xiaotai_net_retry() :
        xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_STATE, "COMPANION_RETRY_UNAVAILABLE");
}

typedef struct {
    char command[NET_CALL_DATA_MAX];
    uint32_t ticket;
    bool rollback, armed, reported;
} command_t;

static bool command_next(void *ctx, const xiaotai_reply_t *reply)
{
    command_t *job = ctx;
    char text[NET_CALL_DATA_MAX];
    const xiaotai_err_t err = xiaotai_net_coprocessor_command(job->command, job->rollback, text, sizeof(text));
    if (err != XIAOTAI_OK) {
        (void)xiaotai_at_fail(reply, err, NULL);
        return false;
    }
    if (job->rollback) {
        job->armed = true;
        xiaotai_at_printf(reply, "+COMPANION: rollback=requested");
        job->reported = xiaotai_at_stream_result(reply) == XIAOTAI_OK;
    } else {
        char hex[2 * NET_CALL_DATA_MAX + 1];
        (void)xiaotai_at_text_encode(text, hex, sizeof(hex));
        xiaotai_at_printf(reply, "+COMPANION-REPLY: text_hex=%s", hex);
    }
    return false;
}

static void command_release(void *ctx)
{
    command_t *job = ctx;
    if (job->armed) {
        /* After the source took the reply, not merely after formatting it.
         * If it disconnected, the worker's bounded fallback still restarts. */
        if (job->reported) xiaotai_net_coprocessor_restart_now();
    } else {
        release_quiesce(job->ticket);
    }
    xiaotai_mem_free(job);
}

static xiaotai_err_t command_stream(const char *word, bool rollback, const xiaotai_reply_t *reply)
{
    const size_t length = strnlen(word, NET_CALL_DATA_MAX);
    if (!length || length == NET_CALL_DATA_MAX) return XIAOTAI_ERR_INVALID_ARG;
    if (!rollback && strcmp(word, "ROLLBACK") == 0) return XIAOTAI_ERR_NOT_ALLOWED;
    if (rollback && !xiaotai_at_reply_is_console(reply)) return XIAOTAI_ERR_NOT_SUPPORTED;
    command_t *job = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*job));
    if (!job) return XIAOTAI_ERR_NO_MEM;
    if (rollback) {
        const xiaotai_err_t err = quiesce(&job->ticket, reply);
        if (err != XIAOTAI_OK) {
            command_release(job);
            return err;
        }
    }
    job->rollback = rollback;
    memcpy(job->command, word, length + 1);
    const xiaotai_at_stream_t stream = {.next = command_next, .release = command_release, .ctx = job};
    const xiaotai_err_t err = xiaotai_at_stream(reply, &stream);
    if (err != XIAOTAI_OK) command_release(job);
    return err;
}

static xiaotai_err_t send(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    return command_stream(argv[0], false, reply);
}

static xiaotai_err_t rollback(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    return command_stream("ROLLBACK", true, reply);
}

#define UPDATE_URL_MAX 192U
#define UPDATE_TIMEOUT_MS 10000U
#define PROGRESS_FENCE_MS 50U

typedef struct {
    uint32_t ticket;
    char url[UPDATE_URL_MAX];
    uint8_t expected[XIAOTAI_SHA256_BYTES];
    uint8_t *image;
} update_t;

typedef struct {
    update_t *job;
    xiaotai_http_response_t *response;
    size_t occupied;
} image_sink_t;

static bool update_url(const char *url)
{
    static const char scheme[] = "http://";
    if (strncmp(url, scheme, sizeof(scheme) - 1) || !url[sizeof(scheme) - 1] ||
        url[sizeof(scheme) - 1] == '/' || strnlen(url, UPDATE_URL_MAX) == UPDATE_URL_MAX)
        return false;
    return valid_field(url) && !strpbrk(url, "@?\\");
}

static xiaotai_err_t receive_image(void *ctx, const uint8_t *data, size_t size)
{
    image_sink_t *sink = ctx;
    const xiaotai_http_response_t *response = sink->response;
    if (response->status != 200 || response->redirect) return XIAOTAI_FAIL;
    if (response->content_length <= 0 || response->content_length > XIAOTAI_NET_COPROCESSOR_IMAGE_MAX)
        return XIAOTAI_ERR_INVALID_SIZE;
    const size_t declared = (size_t)response->content_length;
    if (sink->occupied > declared || size > declared - sink->occupied) return XIAOTAI_ERR_INVALID_SIZE;
    if (!sink->job->image) {
        sink->job->image = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, declared);
        if (!sink->job->image) return XIAOTAI_ERR_NO_MEM;
    }
    memcpy(sink->job->image + sink->occupied, data, size);
    sink->occupied += size;
    return XIAOTAI_OK;
}

static void notify_progress(const char *stage, unsigned percent, xiaotai_err_t err)
{
    (void)xiaotai_at_notifyf("+COMPANION-UPDATE: stage=%s percent=%u error=%d", stage, percent, err);
}

static void close_progress(void *unused)
{
    (void)unused;
    xiaotai_critical_enter(&s_progress_lock);
    atomic_store(&s_show_progress, false);
    xiaotai_critical_exit(&s_progress_lock);
}

static void begin_progress(void)
{
    xiaotai_critical_enter(&s_progress_lock);
    s_last_notice = (xiaotai_net_prepare_t){0};
    atomic_store(&s_show_progress, true);
    xiaotai_critical_exit(&s_progress_lock);
}

static void finish_progress(void)
{
    if (!atomic_load(&s_show_progress)) return;
    /* PREPARE events and this marker share the owner FIFO. Give published
     * stages a chance to enter the AT queue before posting the final result. */
    if (xiaotai_run(close_progress, NULL) == XIAOTAI_OK) {
        const int64_t deadline = xiaotai_time_us() + PROGRESS_FENCE_MS * 1000;
        while (atomic_load(&s_show_progress) && xiaotai_time_us() < deadline) xiaotai_delay_ms(1);
    }
    close_progress(NULL);
}

static xiaotai_err_t stage_image(update_t *job, size_t *length)
{
    xiaotai_http_t *client = NULL;
    xiaotai_err_t err = xiaotai_http_create(&(xiaotai_http_options_t){.rx_buffer_bytes = 2048, .tx_buffer_bytes = 512},
                                           &client);
    if (err != XIAOTAI_OK) return err;
    xiaotai_http_response_t response = {.sink = receive_image, .sink_limit = XIAOTAI_NET_COPROCESSOR_IMAGE_MAX};
    image_sink_t sink = {.job = job, .response = &response};
    response.sink_ctx = &sink;
    const xiaotai_http_request_t request = {.method = XIAOTAI_HTTP_GET, .url = job->url, .timeout_ms = UPDATE_TIMEOUT_MS};
    err = xiaotai_http_perform(client, &request, &response);
    xiaotai_http_destroy(client);
    if (err != XIAOTAI_OK) return err;
    if (response.status != 200 || response.redirect) return XIAOTAI_FAIL;
    if (!sink.occupied || sink.occupied != response.size || response.content_length != (int64_t)sink.occupied)
        return XIAOTAI_ERR_INVALID_SIZE;
    notify_progress("verifying", 0, XIAOTAI_OK);
    uint8_t actual[XIAOTAI_SHA256_BYTES];
    err = xiaotai_sha256(job->image, sink.occupied, actual);
    if (err == XIAOTAI_OK && memcmp(actual, job->expected, sizeof(actual))) err = XIAOTAI_ERR_INVALID_CRC;
    if (err == XIAOTAI_OK) *length = sink.occupied;
    return err;
}

static xiaotai_err_t report_terminal(xiaotai_err_t result, int64_t until_us)
{
    char line[80];
    const bool success = result == XIAOTAI_OK;
    const int length = snprintf(line, sizeof(line), "+COMPANION-UPDATE: stage=%s percent=%u error=%d",
                                success ? "done" : "failed", success ? 100U : 0U, result);
    if (length < 0 || (size_t)length >= sizeof(line)) return XIAOTAI_ERR_INVALID_SIZE;
    int64_t left_us;
    while ((left_us = until_us - xiaotai_time_us()) > 0) {
        const xiaotai_err_t sent = xiaotai_at_notify_wait(line, (uint32_t)((left_us + 999) / 1000));
        /* Only a full ring proves the record was never enqueued. After an
         * output failure or a timeout it may already have reached a source. */
        if (sent != XIAOTAI_ERR_NO_MEM) return sent;
        xiaotai_delay_ms(1);
    }
    return XIAOTAI_ERR_TIMEOUT;
}

static void update_done(xiaotai_err_t result, void *ctx)
{
    update_t *job = ctx;
    const int64_t until_us = xiaotai_time_us() + (int64_t)XIAOTAI_NET_COPROCESSOR_REPLY_WAIT_MS * 1000;
    finish_progress();
    xiaotai_mem_free(job->image);
    uint32_t ticket = job->ticket;
    xiaotai_mem_free(job);
    const xiaotai_err_t delivered = report_terminal(result, until_us);
    if (delivered != XIAOTAI_OK)
        XIAOTAI_LOGW("net", "coprocessor terminal report output: 0x%x (update: 0x%x)",
                    (unsigned)delivered, (unsigned)result);
    /* The worker restarts after successful activation, never as an output error. */
    if (result != XIAOTAI_OK) release_quiesce(ticket);
}

static void download(void *ctx)
{
    update_t *job = ctx;
    notify_progress("downloading", 0, XIAOTAI_OK);
    size_t size = 0;
    xiaotai_err_t err = stage_image(job, &size);
    if (err == XIAOTAI_OK) {
        begin_progress();
        err = net_coprocessor_update_reserved(job->image, size, update_done, job);
    } else {
        net_gate_coprocessor_end();
    }
    if (err != XIAOTAI_OK) update_done(err, job);
    xiaotai_task_exit();
}

static xiaotai_err_t update(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    if (!xiaotai_at_reply_is_console(reply)) return XIAOTAI_ERR_NOT_SUPPORTED;
    uint8_t digest[XIAOTAI_SHA256_BYTES];
    size_t decoded;
    if (!update_url(argv[0]) || strlen(argv[1]) != sizeof(digest) * 2 ||
        xiaotai_at_hex_decode(argv[1], digest, sizeof(digest), &decoded) != XIAOTAI_OK || decoded != sizeof(digest))
        return XIAOTAI_ERR_INVALID_ARG;
    if (xiaotai_activity_current(NULL) || xiaotai_quiesced())
        return xiaotai_at_fail(reply, XIAOTAI_ERR_BUSY, "ACTIVITY");
    if (!xiaotai_mem_has_external()) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (net_state_now() == XIAOTAI_NET_STOPPED)
        return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_STATE, "NOT_STARTED");
    if (net_state_now() != XIAOTAI_NET_UP) return XIAOTAI_ERR_INVALID_STATE;
    uint32_t ticket = 0;
    xiaotai_err_t err = net_gate_coprocessor_write_begin();
    if (err != XIAOTAI_OK) return xiaotai_at_fail(reply, err, "MAINTENANCE");
    err = quiesce(&ticket, reply);
    if (err != XIAOTAI_OK) {
        net_gate_coprocessor_end();
        return err;
    }
    update_t *job = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*job));
    if (job) {
        job->ticket = ticket;
        memcpy(job->url, argv[0], strlen(argv[0]) + 1);
        memcpy(job->expected, digest, sizeof(digest));
        const xiaotai_task_config_t task = {
            .name = "xiaotai_cp", .entry = download, .arg = job,
            .stack_bytes = 8192, .stack = XIAOTAI_MEM_BULK, .priority = 5, .core = XIAOTAI_CORE_ANY,
        };
        xiaotai_task_t handle;
        err = xiaotai_task_create(&task, &handle);
        if (err == XIAOTAI_OK) return XIAOTAI_OK;
        xiaotai_mem_free(job);
    } else {
        err = XIAOTAI_ERR_NO_MEM;
    }
    net_gate_coprocessor_end();
    release_quiesce(ticket);
    return err;
}

static void prepare_event(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (!atomic_load(&s_show_progress) || event->size != sizeof(xiaotai_net_prepare_t)) return;
    static const char *const names[] = {
        [XIAOTAI_NET_PREPARE_CHECKING] = "checking", [XIAOTAI_NET_PREPARE_VERIFYING] = "verifying",
        [XIAOTAI_NET_PREPARE_WRITING] = "writing", [XIAOTAI_NET_PREPARE_ACTIVATING] = "activating",
        [XIAOTAI_NET_PREPARE_RESTARTING] = "restarting",
    };
    const xiaotai_net_prepare_t *progress = event->data;
    const unsigned stage = (unsigned)progress->stage;
    if (stage < sizeof(names) / sizeof(names[0]) && names[stage]) {
        char line[XIAOTAI_AT_NOTIFY_MAX + 1];
        const int used = snprintf(line, sizeof(line), "+COMPANION-UPDATE: stage=%s percent=%u error=%d",
                                  names[stage], (unsigned)progress->percent, progress->error);
        if (used <= 0 || (size_t)used >= sizeof(line)) return;
        xiaotai_critical_enter(&s_progress_lock);
        const bool changed = s_last_notice.stage != progress->stage || s_last_notice.percent != progress->percent ||
                             s_last_notice.error != progress->error;
        if (atomic_load(&s_show_progress) && changed && xiaotai_at_notify(line) == XIAOTAI_OK)
            s_last_notice = *progress;
        xiaotai_critical_exit(&s_progress_lock);
    }
}

xiaotai_err_t net_companion_at_register(const xiaotai_network_t *part)
{
    if (!part) return XIAOTAI_OK;
    size_t count = 0;
    if (part->coprocessor_info) {
        s_verbs[count++] = (xiaotai_at_verb_t){.run = cached};
        s_verbs[count++] = (xiaotai_at_verb_t){.word = "LIVE", .run = live};
    }
    if (part->prepare) s_verbs[count++] = (xiaotai_at_verb_t){.word = "RETRY", .run = retry};
    if (part->coprocessor_command) {
        s_verbs[count++] = (xiaotai_at_verb_t){.word = "SEND", .min_args = 1, .max_args = 1, .run = send, .usage = "<word>"};
        s_verbs[count++] = (xiaotai_at_verb_t){.word = "ROLLBACK", .run = rollback};
    }
    if (part->coprocessor_update) {
        s_verbs[count++] = (xiaotai_at_verb_t){.word = "UPDATE", .min_args = 2, .max_args = 2,
            .run = update, .usage = "<url> <sha256-hex>"};
        if (!s_progress_initialized) {
            xiaotai_critical_init(&s_progress_lock);
            s_progress_initialized = true;
        }
        const xiaotai_err_t err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_NET_PREPARE, prepare_event, NULL);
        if (err != XIAOTAI_OK) return err;
    }
    return count ? xiaotai_at_verbs_register("AT+COMPANION", s_verbs, count) : XIAOTAI_OK;
}

void net_companion_at_reset(void)
{
    atomic_store(&s_quiesce_release, 0);
    atomic_store(&s_release_posted, false);
    atomic_store(&s_show_progress, false);
}
