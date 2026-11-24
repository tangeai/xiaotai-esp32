// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_voice_reliability.py::test_ack
#include "feedback_internal.h"
#include "xiaotai/at.h"
#include "xiaotai/framework.h"
#ifdef FEEDBACK_ADAPTER
#include "xiaotai/media.h"
#endif

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

void feedback_fixture_outputs(xiaotai_output_t *parts, size_t size);

static xiaotai_signal_t ready, alive, unblock;
static atomic_uint now_ms;
static uint32_t timer_id, timer_due;
static xiaotai_timer_fn_t timer_cb;
static void *timer_ctx;
static struct {
    unsigned count, attempts, stops;
    uint32_t token, generation, last_stopped;
    uint16_t repeat, gap;
    const char *names[XIAOTAI_FEEDBACK_SEQUENCE_MAX];
    bool busy, mute;
    uint32_t current_generation;
} mock;

int64_t __wrap_xiaotai_time_us(void) { return (int64_t)atomic_load(&now_ms) * 1000; }

xiaotai_err_t __wrap_xiaotai_timer_start(uint32_t delay, xiaotai_timer_fn_t fn, void *ctx, uint32_t *id)
{
    assert(xiaotai_on_owner() && !timer_cb);
    timer_due = atomic_load(&now_ms) + delay;
    timer_ctx = ctx;
    timer_cb = fn;
    *id = ++timer_id;
    return XIAOTAI_OK;
}

void __wrap_xiaotai_timer_cancel(uint32_t id)
{
    assert(xiaotai_on_owner());
    if (id == timer_id) timer_cb = NULL;
}

static xiaotai_err_t fake_play(void *ctx, const xiaotai_feedback_play_t *play)
{
    (void)ctx;
    assert(xiaotai_on_owner() && play->count && play->count <= XIAOTAI_FEEDBACK_SEQUENCE_MAX);
    ++mock.attempts;
    if (mock.busy) return XIAOTAI_ERR_BUSY;
    if (mock.mute || (play->generation && play->generation != mock.current_generation)) return XIAOTAI_ERR_INVALID_STATE;
    for (size_t i = 0; i < play->count; ++i) {
        if (!strcmp(play->names[i], "absent")) return XIAOTAI_ERR_NOT_FOUND;
        mock.names[i] = play->names[i];
    }
    mock.count = (unsigned)play->count;
    mock.token = play->token;
    mock.repeat = play->repeat;
    mock.gap = play->gap_ms;
    mock.generation = play->generation;
    return XIAOTAI_OK;
}

static void fake_stop(void *ctx, uint32_t token)
{
    (void)ctx;
    assert(xiaotai_on_owner() && token && token == mock.token);
    mock.last_stopped = token;
    ++mock.stops;
}

#ifdef FEEDBACK_ADAPTER
static xiaotai_media_play_fn_t media_cb;
static void *media_ctx;
static uint32_t media_id;

xiaotai_err_t xiaotai_media_play_clips(const xiaotai_media_play_t *request, uint32_t *id)
{
    assert(request->priority == XIAOTAI_PLAY_NORMAL && request->flags == 0);
    const char *names[XIAOTAI_FEEDBACK_SEQUENCE_MAX];
    for (size_t i = 0; i < request->clip_count; ++i) names[i] = request->clips[i].name;
    const xiaotai_feedback_play_t play = {names, request->clip_count, request->repeat, request->gap_ms,
                                         request->generation, ++media_id};
    const xiaotai_err_t err = fake_play(NULL, &play);
    if (err == XIAOTAI_OK) {
        *id = media_id;
        media_cb = request->done;
        media_ctx = request->ctx;
    }
    return err;
}

void xiaotai_media_stop(uint32_t id) { fake_stop(NULL, id); }
void xiaotai_media_status(xiaotai_media_status_t *out)
{
    *out = (xiaotai_media_status_t){.generation = mock.current_generation, .speaker_muted = mock.mute};
}
#endif

static const xiaotai_feedback_state_t states[] = {
    {.name = "incoming", .priority = 3, .sound = {.name = "ring.incoming", .gap_ms = 560, .group = 1}},
    {.name = "outgoing", .priority = 4, .sound = {.name = "ring.outgoing", .gap_ms = 760}},
};
static const xiaotai_feedback_effect_t effects[] = {
    {.name = "tap", .sound = {.name = "cue.tap", .rank = 1}, .duration_ms = 140,
     .pattern = {.kind = XIAOTAI_LIGHT_SOLID, .level_max = 255, .color = {49, 0, 0}}},
    {.name = "guide", .sound = {.name = "guide.ready", .rank = 2, .group = 1, .flags = XIAOTAI_FEEDBACK_WAIT}},
    {.name = "bind", .sound = {.name = "guide.bind", .rank = 3, .group = 1, .flags = XIAOTAI_FEEDBACK_WAIT}},
    {.name = "urgent", .sound = {.name = "cue.urgent", .rank = 4, .flags = XIAOTAI_FEEDBACK_INTERRUPT}},
    {.name = "missing", .sound = {.name = "absent", .rank = 5}},
    {.name = "slot.a", .sound = {.name = "cue.a", .rank = 2}},
    {.name = "slot.b", .sound = {.name = "cue.b", .rank = 2}},
};
static const xiaotai_feedback_table_t table = {
    states, sizeof(states) / sizeof(states[0]), effects, sizeof(effects) / sizeof(effects[0]),
};

static bool install_player;
static xiaotai_err_t register_table(void)
{
    static const xiaotai_feedback_player_t player = {.play = fake_play, .stop = fake_stop};
    if (install_player) xiaotai_feedback_set_player(&player);
    return xiaotai_feedback_register(&table);
}

static uint8_t red;
static xiaotai_err_t pixel(void *self, uint8_t r, uint8_t g, uint8_t b)
{
    (void)self;
    assert(xiaotai_on_owner() && !g && !b);
    red = r;
    return XIAOTAI_OK;
}

static void give(void *ctx) { xiaotai_signal_give(ctx); }
static void settle(void)
{
    assert(xiaotai_run(give, &ready) == XIAOTAI_OK && xiaotai_signal_take(&ready, 5000));
}

typedef struct { xiaotai_timer_fn_t run; void *ctx; } job_t;
static void run_job(void *ctx)
{
    job_t *job = ctx;
    job->run(job->ctx);
    xiaotai_signal_give(&ready);
}

static void on_owner(xiaotai_timer_fn_t fn, void *ctx)
{
    job_t job = {fn, ctx};
    assert(xiaotai_run(run_job, &job) == XIAOTAI_OK && xiaotai_signal_take(&ready, 5000));
}

static void clock_tick(void *ctx)
{
    const uint32_t now = (uint32_t)(uintptr_t)ctx;
    atomic_store(&now_ms, now);
    assert(timer_cb && (int32_t)(now - timer_due) >= 0);
    const xiaotai_timer_fn_t cb = timer_cb;
    timer_cb = NULL;
    cb(timer_ctx);
}

static void tick_at(uint32_t now) { on_owner(clock_tick, (void *)(uintptr_t)now); }

typedef struct { unsigned calls; xiaotai_err_t status; } answer_t;
static void done(xiaotai_err_t result, void *ctx)
{
    answer_t *a = ctx;
    assert(xiaotai_on_owner());
    ++a->calls;
    a->status = result;
}

typedef struct {
    const char *name;
    xiaotai_feedback_options_t options;
    xiaotai_err_t result;
} fire_t;

static void fire_job(void *ctx)
{
    fire_t *job = ctx;
    job->result = xiaotai_feedback_fire(job->name, &job->options);
}

static xiaotai_err_t fire(const char *name, answer_t *answer, uint32_t valid_ms)
{
    fire_t job = {.name = name, .options = {.done = done, .ctx = answer, .valid_ms = valid_ms}};
    on_owner(fire_job, &job);
    return job.result;
}

static void complete(xiaotai_err_t result)
{
    xiaotai_feedback_player_done(mock.token, result);
    settle();
}

static void slots(void)
{
    answer_t a = {0}, b = {0}, c = {0}, d = {0}, e = {0};
    assert(fire("guide", &a, 0) == XIAOTAI_OK);
    const uint32_t playing = mock.token;
    assert(fire("bind", &b, 0) == XIAOTAI_OK);
    assert(fire("tap", &c, 0) == XIAOTAI_ERR_INVALID_STATE && !c.calls);
    assert(fire("bind", &d, 0) == XIAOTAI_OK);
    assert(b.calls == 1 && b.status == XIAOTAI_ERR_INVALID_STATE && mock.token == playing);
    assert(fire("urgent", &e, 0) == XIAOTAI_OK && mock.last_stopped == playing);
    assert(a.calls == 1 && a.status == XIAOTAI_ERR_INVALID_STATE);
    assert(d.calls == 1 && d.status == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_feedback_player_done(playing, XIAOTAI_ERR_INVALID_STATE);
    complete(XIAOTAI_OK);
    assert(e.calls == 1 && e.status == XIAOTAI_OK && !xiaotai_feedback_sound_pending());
    xiaotai_feedback_stats_t stats;
    xiaotai_feedback_stats(&stats);
    assert(stats.replaced == 2 && stats.dropped_rank == 1 && stats.stale_tokens == 1 && stats.played == 1);
}

static void busy(void)
{
    answer_t a = {0}, b = {0}, queued = {0};
    mock.busy = true;
    assert(fire("tap", &a, 0) == XIAOTAI_ERR_INVALID_STATE && !a.calls);
    const xiaotai_feedback_options_t opts = {.done = done, .ctx = &queued};
    assert(xiaotai_feedback_fire("tap", &opts) == XIAOTAI_OK);
    settle();
    assert(queued.calls == 1 && queued.status == XIAOTAI_ERR_INVALID_STATE);
    const uint32_t start = UINT32_MAX - 49U;
    atomic_store(&now_ms, start);
    assert(fire("guide", &b, 250) == XIAOTAI_OK && timer_due == start + 100U);
    tick_at(start + 100U);
    tick_at(start + 200U);
    assert(timer_due == start + 250U && mock.attempts == 5 && xiaotai_feedback_sound_pending());
    tick_at(start + 250U);
    assert(b.calls == 1 && b.status == XIAOTAI_ERR_TIMEOUT && !xiaotai_feedback_sound_pending());
    b.calls = 0;
    assert(fire("guide", &b, 0) == XIAOTAI_OK);
    mock.busy = false;
    xiaotai_feedback_player_idle();
    settle();
    complete(XIAOTAI_OK);
    assert(b.calls == 1 && b.status == XIAOTAI_OK);
}

static void groups(void)
{
    answer_t a = {0}, b = {0};
    assert(fire("guide", &a, 0) == XIAOTAI_OK && fire("bind", &b, 0) == XIAOTAI_OK);
    assert(xiaotai_feedback_cancel_group(1, false) == XIAOTAI_OK);
    settle();
    assert(!a.calls && b.calls == 1 && b.status == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_feedback_group_enable(1, false) == XIAOTAI_OK);
    settle();
    assert(!xiaotai_feedback_group_enabled(1) && a.calls == 1 && a.status == XIAOTAI_ERR_INVALID_STATE);
    assert(fire("guide", &b, 0) == XIAOTAI_ERR_INVALID_STATE && b.calls == 1);
    fire_t force = {.name = "guide", .options = {.force = true}};
    on_owner(fire_job, &force);
    assert(force.result == XIAOTAI_OK && xiaotai_feedback_sound_pending());
    complete(XIAOTAI_OK);
}

static void loops(void)
{
    assert(xiaotai_feedback_set("incoming", true) == XIAOTAI_OK);
    settle();
    assert(mock.repeat == XIAOTAI_FEEDBACK_FOREVER && mock.gap == 560 && !xiaotai_feedback_sound_pending());
    assert(xiaotai_feedback_set("outgoing", true) == XIAOTAI_OK);
    settle();
    assert(mock.gap == 760 && mock.stops == 1 && !strcmp(mock.names[0], "ring.outgoing"));
    answer_t a = {0};
    assert(fire("guide", &a, 0) == XIAOTAI_OK && mock.repeat == 0 && mock.stops == 2);
    complete(XIAOTAI_OK);
    assert(a.calls == 1 && mock.gap == 760 && mock.repeat == XIAOTAI_FEEDBACK_FOREVER);
    assert(xiaotai_feedback_set("outgoing", false) == XIAOTAI_OK);
    settle();
    assert(mock.gap == 560);
    assert(xiaotai_feedback_group_enable(1, false) == XIAOTAI_OK);
    settle();
    assert(!timer_cb && mock.stops == 4);
    mock.busy = true;
    assert(xiaotai_feedback_group_enable(1, true) == XIAOTAI_OK);
    settle();
    const unsigned attempts = mock.attempts;
    assert(timer_cb);
    tick_at(100);
    assert(mock.attempts == attempts); /* Mailbox delivery is not player polling. */
    mock.busy = false;
    xiaotai_feedback_player_idle();
    settle();
    assert(mock.gap == 560 && !xiaotai_feedback_sound_pending());
    assert(xiaotai_feedback_set("incoming", false) == XIAOTAI_OK);
    settle();
}

static void block(void *ctx)
{
    (void)ctx;
    xiaotai_signal_give(&alive);
    assert(xiaotai_signal_take(&unblock, 5000));
}

static void sequence(void)
{
    answer_t a = {0};
    const char *then[] = {"digit.1", "digit.2", "digit.3", "digit.4", "digit.5", "digit.6"};
    const xiaotai_feedback_options_t opts = {.then = then, .then_count = 6, .done = done, .ctx = &a};
    assert(xiaotai_run(block, NULL) == XIAOTAI_OK && xiaotai_signal_take(&alive, 5000));
    assert(xiaotai_feedback_fire("bind", &opts) == XIAOTAI_OK);
    then[0] = "changed";
    xiaotai_signal_give(&unblock);
    settle();
    assert(mock.count == 7 && !strcmp(mock.names[0], "guide.bind") && !strcmp(mock.names[1], "digit.1") &&
           !strcmp(mock.names[6], "digit.6") && !a.calls);
    complete(XIAOTAI_OK);
    assert(a.calls == 1);
    a.calls = 0;
    assert(fire("missing", &a, 0) == XIAOTAI_OK && a.calls == 1 && a.status == XIAOTAI_ERR_NOT_FOUND);
}

static void generation(void)
{
    answer_t a = {0}, b = {0};
    fire_t request = {.name = "tap", .options = {.generation = 9, .valid_ms = 1000, .done = done, .ctx = &a}};
    mock.current_generation = 8;
    on_owner(fire_job, &request);
    assert(request.result == XIAOTAI_ERR_INVALID_STATE && !a.calls);
    mock.current_generation = 9;
    on_owner(fire_job, &request);
    assert(request.result == XIAOTAI_OK && mock.generation == 9);
    complete(XIAOTAI_ERR_INVALID_STATE);
    assert(a.calls == 1 && a.status == XIAOTAI_ERR_INVALID_STATE);
    assert(fire("guide", &b, 0) == XIAOTAI_OK);
    assert(xiaotai_feedback_fire("bind", &request.options) == XIAOTAI_OK);
    settle();
    assert(a.calls == 1);
    mock.current_generation = 10;
    complete(XIAOTAI_OK);
    assert(a.calls == 2 && a.status == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_feedback_stats_t stats;
    xiaotai_feedback_stats(&stats);
    assert(stats.dropped_stale == 2 && stats.dropped_rank == 0);
}

static void skip(void *ctx) { (void)ctx; }
static void receipts(void)
{
    answer_t a = {0};
    assert(fire("tap", &a, 0) == XIAOTAI_OK);
    const uint32_t token = mock.token;
    assert(xiaotai_run(block, NULL) == XIAOTAI_OK && xiaotai_signal_take(&alive, 5000));
    for (unsigned i = 0; i < 8; ++i) assert(xiaotai_run(skip, NULL) == XIAOTAI_OK);
    assert(xiaotai_run(skip, NULL) == XIAOTAI_ERR_NO_MEM);
    xiaotai_feedback_player_done(token, XIAOTAI_OK);
    xiaotai_feedback_player_done(token, XIAOTAI_FAIL);
    xiaotai_signal_give(&unblock);
    xiaotai_err_t queued;
    do { queued = xiaotai_run(give, &ready); if (queued == XIAOTAI_ERR_NO_MEM) xiaotai_delay_ms(1); }
    while (queued == XIAOTAI_ERR_NO_MEM);
    assert(queued == XIAOTAI_OK && xiaotai_signal_take(&ready, 5000));
    tick_at(100);
    assert(a.calls == 1 && a.status == XIAOTAI_OK && !xiaotai_feedback_sound_pending());
}

static void cancel_queue(bool full)
{
    answer_t playing = {0}, waiting = {0};
    assert(fire("guide", &playing, 0) == XIAOTAI_OK);
    assert(fire("bind", &waiting, 0) == XIAOTAI_OK);
    assert(xiaotai_run(block, NULL) == XIAOTAI_OK && xiaotai_signal_take(&alive, 5000));
    for (unsigned i = 0; i < (full ? 8U : 6U); ++i) assert(xiaotai_run(skip, NULL) == XIAOTAI_OK);
    if (full) assert(xiaotai_run(skip, NULL) == XIAOTAI_ERR_NO_MEM);
    assert(xiaotai_feedback_cancel("not.registered", true) == XIAOTAI_OK);
    assert(xiaotai_feedback_cancel("", true) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_feedback_cancel_group(XIAOTAI_FEEDBACK_GROUPS_MAX + 1U, false) == XIAOTAI_ERR_INVALID_ARG);
    const xiaotai_err_t outcome = full ? XIAOTAI_ERR_NO_MEM : XIAOTAI_OK;
    assert(xiaotai_feedback_cancel_group(1, false) == outcome);
    assert(xiaotai_feedback_cancel("guide", true) == outcome);
    xiaotai_signal_give(&unblock);
    xiaotai_err_t queued = XIAOTAI_ERR_NO_MEM;
    for (size_t i = 0; i < 5000 && queued == XIAOTAI_ERR_NO_MEM; ++i) {
        queued = xiaotai_run(give, &ready);
        if (queued == XIAOTAI_ERR_NO_MEM) xiaotai_delay_ms(1);
    }
    assert(queued == XIAOTAI_OK && xiaotai_signal_take(&ready, 5000));
    if (full) {
        tick_at(100);
        assert(!waiting.calls && !playing.calls && !mock.stops && xiaotai_feedback_sound_pending());
        /* The rejected request has no side effects; retry in the original order. */
        assert(xiaotai_feedback_cancel_group(1, false) == XIAOTAI_OK);
        assert(xiaotai_feedback_cancel("guide", true) == XIAOTAI_OK);
        settle();
    }
    assert(waiting.calls == 1 && waiting.status == XIAOTAI_ERR_INVALID_STATE);
    assert(playing.calls == 1 && playing.status == XIAOTAI_ERR_INVALID_STATE && mock.stops == 1);
    assert(!xiaotai_feedback_sound_pending());
}

typedef struct { answer_t first, replay; } replay_t;
static void replay_after_cancel(xiaotai_err_t result, void *ctx)
{
    replay_t *a = ctx;
    done(result, &a->first);
    const xiaotai_feedback_options_t opts = {.done = done, .ctx = &a->replay};
    assert(xiaotai_feedback_fire("tap", &opts) == XIAOTAI_OK);
}

static void cancel_semantics(void)
{
    answer_t playing = {0}, waiting = {0};
    assert(fire("guide", &playing, 0) == XIAOTAI_OK);
    assert(fire("bind", &waiting, 0) == XIAOTAI_OK);
    assert(xiaotai_feedback_cancel("guide", true) == XIAOTAI_OK);
    assert(xiaotai_feedback_cancel_group(1, false) == XIAOTAI_OK);
    settle();
    assert(playing.calls == 1 && playing.status == XIAOTAI_ERR_INVALID_STATE);
    assert(!waiting.calls && xiaotai_feedback_sound_pending());
    complete(XIAOTAI_OK);
    assert(waiting.calls == 1 && waiting.status == XIAOTAI_OK);

    replay_t a = {0};
    fire_t prompt = {.name = "tap", .options = {.done = replay_after_cancel, .ctx = &a}};
    on_owner(fire_job, &prompt);
    assert(prompt.result == XIAOTAI_OK);
    assert(xiaotai_feedback_cancel("tap", true) == XIAOTAI_OK);
    assert(xiaotai_feedback_cancel("tap", true) == XIAOTAI_OK);
    settle();
    assert(a.first.calls == 1 && a.first.status == XIAOTAI_ERR_INVALID_STATE);
    assert(a.replay.calls == 1 && a.replay.status == XIAOTAI_ERR_INVALID_STATE);
    assert(!xiaotai_feedback_sound_pending());
}

typedef struct result_trace result_trace_t;
typedef struct { result_trace_t *trace; const char *effect; char label; } trace_prompt_t;
struct result_trace { char words[9]; size_t size; bool replay; };

static void trace_cancel(xiaotai_err_t result, void *ctx)
{
    trace_prompt_t *prompt = ctx;
    result_trace_t *trace = prompt->trace;
    assert(xiaotai_on_owner() && result == XIAOTAI_ERR_INVALID_STATE);
    if (!trace->replay) return;
    assert(trace->size + 1 < sizeof(trace->words));
    trace->words[trace->size++] = prompt->label;
    const xiaotai_feedback_options_t opts = {.done = trace_cancel, .ctx = prompt};
    assert(xiaotai_feedback_fire(prompt->effect, &opts) == XIAOTAI_OK);
}

static void cancel_word(void)
{
    result_trace_t trace = {.replay = true};
    trace_prompt_t prompts[2] = {{&trace, "slot.a", 'a'}, {&trace, "slot.b", 'b'}};
    for (size_t i = 0; i < 2; ++i) {
        fire_t request = {.name = prompts[i].effect, .options = {.done = trace_cancel, .ctx = &prompts[i]}};
        on_owner(fire_job, &request);
        assert(request.result == XIAOTAI_OK);
    }
    const char *const words[] = {"aabbabab", "abbaabba"};
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        const char *word = words[i];
        trace.size = 0;
        assert(xiaotai_run(block, NULL) == XIAOTAI_OK && xiaotai_signal_take(&alive, 5000));
        for (size_t n = 0; word[n]; ++n) {
            assert(xiaotai_feedback_cancel(prompts[word[n] == 'b'].effect, true) == XIAOTAI_OK);
        }
        assert(xiaotai_run(skip, NULL) == XIAOTAI_ERR_NO_MEM);
        xiaotai_signal_give(&unblock);
        xiaotai_err_t queued;
        do { queued = xiaotai_run(give, &ready); if (queued == XIAOTAI_ERR_NO_MEM) xiaotai_delay_ms(1); }
        while (queued == XIAOTAI_ERR_NO_MEM);
        assert(queued == XIAOTAI_OK && xiaotai_signal_take(&ready, 5000));
        assert(trace.size == strlen(word) && !strcmp(trace.words, word));
        printf("cancel sequence: %s\n", trace.words);
    }
    trace.replay = false;
    assert(xiaotai_feedback_cancel(NULL, true) == XIAOTAI_OK);
    settle();
    assert(!xiaotai_feedback_sound_pending());
}

static void no_media(void)
{
    answer_t a = {0};
    assert(fire("tap", &a, 0) == XIAOTAI_OK && a.calls == 1 && a.status == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(red == 49 && mock.attempts == 0 && !xiaotai_feedback_sound_pending());
    tick_at(140);
    assert(red == 0);
}

static void teardown(void)
{
    answer_t a = {0}, b = {0};
    assert(fire("guide", &a, 0) == XIAOTAI_OK && fire("bind", &b, 120) == XIAOTAI_OK);
    const uint32_t playing = mock.token;
    assert(timer_cb && xiaotai_feedback_sound_pending());
    xiaotai_feedback_reset();
    assert(!timer_cb && mock.stops == 1 && mock.last_stopped == playing);
    assert(xiaotai_feedback_cancel("guide", true) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_feedback_cancel("not.registered", true) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_feedback_cancel_group(1, true) == XIAOTAI_ERR_INVALID_STATE);
    assert(a.calls == 1 && a.status == XIAOTAI_ERR_INVALID_STATE);
    assert(b.calls == 1 && b.status == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_feedback_player_done(playing, XIAOTAI_OK);
    settle();
    assert(a.calls == 1 && !xiaotai_feedback_sound_pending());
}

static char text[1600];
static void write_reply(void *ctx, const char *line)
{
    (void)ctx;
    const size_t used = strlen(text);
    int n = snprintf(text + used, sizeof(text) - used, "%s\n", line);
    assert(n >= 0 && (size_t)n < sizeof(text) - used);
}

static void commands(void)
{
    const xiaotai_reply_t reply = {.write = write_reply};
    assert(xiaotai_command_run("AT+FEEDBACK", &reply, 5000) == XIAOTAI_OK);
    assert(strstr(text, "shown=none loop=none playing=none waiting=none groups=ff") && strstr(text, "skipped_outputs="));
    assert(xiaotai_command_run("AT+FEEDBACK SET incoming 1", &reply, 5000) == XIAOTAI_OK);
    assert(xiaotai_command_run("AT+FEEDBACK STATES", &reply, 5000) == XIAOTAI_OK);
    assert(strstr(text, "name=incoming priority=3 active=1"));
    assert(xiaotai_command_run("AT+FEEDBACK GROUP 1 0", &reply, 5000) == XIAOTAI_OK);
    assert(xiaotai_command_run("AT+FEEDBACK FIRE guide", &reply, 5000) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_command_run("AT+FEEDBACK FIRE guide FORCE", &reply, 5000) == XIAOTAI_OK);
    assert(xiaotai_command_run("AT+FEEDBACK CANCEL", &reply, 5000) == XIAOTAI_OK && !xiaotai_feedback_sound_pending());
}

#ifdef FEEDBACK_ADAPTER
static void media_finish(void *ctx)
{
    (void)ctx;
    media_cb(mock.token, XIAOTAI_OK, media_ctx);
}

static void adapter(void)
{
    answer_t a = {0};
    mock.busy = true;
    assert(fire("bind", &a, 250) == XIAOTAI_OK && !a.calls && mock.attempts == 1);
    mock.busy = false;
    assert(xiaotai_event_emit(XIAOTAI_EVENT_MEDIA_OUTPUT_FREE, NULL, NULL, 0) == XIAOTAI_OK);
    settle();
    assert(mock.attempts == 2 && mock.count == 1 && !strcmp(mock.names[0], "guide.bind"));
    on_owner(media_finish, NULL);
    assert(a.calls == 1 && a.status == XIAOTAI_OK);
    assert(xiaotai_feedback_set("incoming", true) == XIAOTAI_OK);
    settle();
    assert(mock.gap == 560 && mock.repeat == XIAOTAI_PLAY_FOREVER);
    assert(xiaotai_feedback_set("incoming", false) == XIAOTAI_OK);
    settle();
    assert(mock.last_stopped == mock.token);
    mock.current_generation = 9;
    mock.mute = true;
    fire_t tagged = {.name = "tap", .options = {.generation = 9}};
    on_owner(fire_job, &tagged);
    assert(tagged.result == XIAOTAI_ERR_INVALID_STATE);
    tagged.options.generation = 8;
    on_owner(fire_job, &tagged);
    xiaotai_feedback_stats_t stats;
    xiaotai_feedback_stats(&stats);
    assert(stats.dropped_stale == 1 && stats.player_errors == 1);
}
#endif

int main(int argc, char **argv)
{
    static xiaotai_output_t light = {.desc = {.kind = XIAOTAI_OUTPUT_LIGHT, .role = "status", .pixels = 1}, .set = pixel};
#ifdef FEEDBACK_ADAPTER
    const char *fallback = "adapter";
#else
    const char *fallback = "slots";
#endif
    const char *test = argc > 1 ? argv[1] : fallback;
    const bool lamp = !strcmp(test, "no_media");
    feedback_fixture_outputs(&light, lamp ? 1 : 0);
    assert(xiaotai_hal_init() == XIAOTAI_OK);
    install_player = !lamp && strcmp(test, "adapter");
    assert(xiaotai_signal_init(&ready) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&alive) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&unblock) == XIAOTAI_OK);
    static const xiaotai_module_t engine = {.name = "feedback", .init = xiaotai_engine_feedback_init,
                                          .start = xiaotai_engine_feedback_start};
    static const xiaotai_application_t app = {.name = "fixture", .init = register_table};
    const xiaotai_module_t *const modules[] = {&engine};
    const xiaotai_application_t *const apps[] = {&app};
    const xiaotai_framework_config_t config = {.modules = modules, .module_count = 1,
        .applications = apps, .application_count = 1, .queue_depth = 8};
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    if (!strcmp(test, "slots")) slots();
    else if (!strcmp(test, "busy")) busy();
    else if (!strcmp(test, "groups")) groups();
    else if (!strcmp(test, "loop")) loops();
    else if (!strcmp(test, "sequence")) sequence();
    else if (!strcmp(test, "generation")) generation();
    else if (!strcmp(test, "done")) receipts();
    else if (!strcmp(test, "cancel_queue_full")) cancel_queue(true);
    else if (!strcmp(test, "cancel_fifo")) cancel_queue(false);
    else if (!strcmp(test, "cancel_semantics")) cancel_semantics();
    else if (!strcmp(test, "cancel_word")) cancel_word();
    else if (lamp) no_media();
    else if (!strcmp(test, "teardown")) teardown();
    else if (!strcmp(test, "commands")) commands();
#ifdef FEEDBACK_ADAPTER
    else if (!strcmp(test, "adapter")) adapter();
#endif
    else assert(false);
    xiaotai_feedback_reset();
    xiaotai_framework_reset();
    xiaotai_at_reset();
    xiaotai_signal_deinit(&unblock);
    xiaotai_signal_deinit(&alive);
    xiaotai_signal_deinit(&ready);
    return 0;
}
