// SPDX-License-Identifier: MIT
/* Shared fakes of the video engine's host tests (see fakes.h). */
#include "fakes.h"

#include <stdatomic.h>
#include <string.h>

/* ================================================================ hardware */

static fake_camera_t s_cameras[FAKE_CAMERAS_MAX];
static size_t s_camera_count;
static xiaotai_display_t s_displays[FAKE_DISPLAYS_MAX];
static size_t s_display_count;

const xiaotai_camera_t *xiaotai_hal_camera(size_t index)
{
    return index < s_camera_count ? &s_cameras[index].part : NULL;
}

const xiaotai_display_t *xiaotai_hal_display(size_t index)
{
    return index < s_display_count ? &s_displays[index] : NULL;
}

void fake_hal_reset(void)
{
    memset(s_cameras, 0, sizeof(s_cameras));
    memset(s_displays, 0, sizeof(s_displays));
    s_camera_count = 0;
    s_display_count = 0;
}

static void camera_log(fake_camera_t *camera, fake_camera_call_t call)
{
    if (camera->log_count < FAKE_CAMERA_LOG_MAX) camera->log[camera->log_count++] = call;
}

static xiaotai_err_t camera_prepare(void *self)
{
    fake_camera_t *camera = self;
    camera_log(camera, FAKE_CAMERA_PREPARE);
    return camera->prepare_result;
}

static xiaotai_err_t camera_set_power(void *self, bool enabled)
{
    fake_camera_t *camera = self;
    camera_log(camera, enabled ? FAKE_CAMERA_POWER_ON : FAKE_CAMERA_POWER_OFF);
    return camera->power_result;
}

static xiaotai_err_t camera_open(void *self, const xiaotai_camera_mode_t *mode)
{
    (void)mode;
    fake_camera_t *camera = self;
    camera_log(camera, FAKE_CAMERA_OPEN);
    return camera->open_result;
}

static xiaotai_err_t camera_close(void *self)
{
    fake_camera_t *camera = self;
    camera_log(camera, FAKE_CAMERA_CLOSE);
    return camera->close_result;
}

static xiaotai_err_t camera_borrow(void *self, xiaotai_camera_frame_t *frame)
{
    (void)frame;
    fake_camera_t *camera = self;
    camera_log(camera, FAKE_CAMERA_BORROW);
    return camera->borrow_result ? camera->borrow_result : XIAOTAI_ERR_NOT_FOUND;
}

static xiaotai_err_t camera_release(void *self, xiaotai_camera_frame_t *frame)
{
    (void)frame;
    fake_camera_t *camera = self;
    camera_log(camera, FAKE_CAMERA_RELEASE);
    return XIAOTAI_OK;
}

fake_camera_t *fake_camera_add(const xiaotai_camera_desc_t *desc, bool with_prepare)
{
    CHECK(s_camera_count < FAKE_CAMERAS_MAX, "too many fake cameras");
    fake_camera_t *camera = &s_cameras[s_camera_count++];
    memset(camera, 0, sizeof(*camera));
    camera->part.desc = *desc;
    camera->part.prepare = with_prepare ? camera_prepare : NULL;
    camera->part.set_power = camera_set_power;
    camera->part.open = camera_open;
    camera->part.close = camera_close;
    camera->part.borrow = camera_borrow;
    camera->part.release = camera_release;
    camera->part.self = camera;
    return camera;
}

xiaotai_display_t *fake_display_add(uint16_t width, uint16_t height, xiaotai_pixel_t pixel, uint32_t flags)
{
    CHECK(s_display_count < FAKE_DISPLAYS_MAX, "too many fake displays");
    xiaotai_display_t *display = &s_displays[s_display_count++];
    memset(display, 0, sizeof(*display));
    display->desc.width = width;
    display->desc.height = height;
    display->desc.pixel = pixel;
    display->desc.flags = flags;
    return display;
}

/* ================================================================ backends */

fake_backend_t fake_encoder_ctl[FAKE_BACKENDS];
fake_backend_t fake_decoder_ctl[FAKE_BACKENDS];
fake_backend_t fake_transform_ctl[FAKE_BACKENDS];
xiaotai_video_encoder_t fake_encoders[FAKE_BACKENDS];
xiaotai_video_decoder_t fake_decoders[FAKE_BACKENDS];
xiaotai_video_transform_t fake_transforms[FAKE_BACKENDS];

static int encoder_match(size_t i, const xiaotai_video_encoder_config_t *config)
{
    fake_backend_t *b = &fake_encoder_ctl[i];
    ++b->match_calls;
    b->last_encoder = *config;
    if (b->codec != XIAOTAI_VIDEO_CODEC_NONE && config->codec != b->codec) return 0;
    if (b->pixel != XIAOTAI_VIDEO_PIXEL_NONE && config->pixel != b->pixel) return 0;
    if (b->max_width && config->width > b->max_width) return 0;
    if (b->score > 0) b->matched_encoder = *config;
    return b->score;
}

static xiaotai_err_t encoder_open(size_t i, const xiaotai_video_encoder_config_t *config, void **state)
{
    fake_backend_t *b = &fake_encoder_ctl[i];
    ++b->open_calls;
    b->last_encoder = *config;
    *state = b->open_result == XIAOTAI_OK ? b : NULL;
    return b->open_result;
}

static void encoder_close(size_t i) { ++fake_encoder_ctl[i].close_calls; }

static xiaotai_err_t encoder_encode(void *state, const xiaotai_video_image_t *image, bool force_key,
                                   xiaotai_video_packet_t *packet)
{
    (void)state;
    (void)image;
    (void)force_key;
    (void)packet;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

static int decoder_match(size_t i, const xiaotai_video_decoder_config_t *config)
{
    fake_backend_t *b = &fake_decoder_ctl[i];
    ++b->match_calls;
    b->last_decoder = *config;
    if (b->codec != XIAOTAI_VIDEO_CODEC_NONE && config->codec != b->codec) return 0;
    return b->score;
}

static xiaotai_err_t decoder_open(size_t i, const xiaotai_video_decoder_config_t *config, void **state)
{
    fake_backend_t *b = &fake_decoder_ctl[i];
    ++b->open_calls;
    b->last_decoder = *config;
    *state = b->open_result == XIAOTAI_OK ? b : NULL;
    return b->open_result;
}

static void decoder_close(size_t i, void *state)
{
    fake_backend_t *b = &fake_decoder_ctl[i];
    ++b->close_calls;
    b->state = state;
}

static xiaotai_err_t decoder_decode(void *state, const xiaotai_video_packet_t *packet, uint8_t *out, size_t capacity,
                                    xiaotai_video_image_t *image)
{
    (void)state;
    (void)packet;
    (void)out;
    (void)capacity;
    (void)image;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

static int transform_match(size_t i, const xiaotai_video_transform_config_t *config)
{
    fake_backend_t *b = &fake_transform_ctl[i];
    ++b->match_calls;
    if (b->from != XIAOTAI_VIDEO_PIXEL_NONE && config->in_pixel != b->from) return 0;
    if (b->pixel != XIAOTAI_VIDEO_PIXEL_NONE && config->out_pixel != b->pixel) return 0;
    return b->score;
}

static bool transform_limits(size_t i, xiaotai_video_pixel_t in, xiaotai_video_pixel_t out,
                             xiaotai_video_scale_limits_t *limits)
{
    const fake_backend_t *b = &fake_transform_ctl[i];
    if (b->from != XIAOTAI_VIDEO_PIXEL_NONE && in != b->from) return false;
    if (b->pixel != XIAOTAI_VIDEO_PIXEL_NONE && out != b->pixel) return false;
    limits->denominator = 16U;
    limits->step = 1U;
    limits->max = 16U;
    return true;
}

static xiaotai_err_t transform_open(size_t i, const xiaotai_video_transform_config_t *config, void **state)
{
    (void)config;
    fake_backend_t *b = &fake_transform_ctl[i];
    ++b->open_calls;
    *state = b->open_result == XIAOTAI_OK ? b : NULL;
    return b->open_result;
}

static xiaotai_err_t transform_run(void *state, const uint8_t *in, size_t in_size, uint8_t *out, size_t out_capacity)
{
    (void)state;
    (void)in;
    (void)in_size;
    (void)out;
    (void)out_capacity;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

static void transform_close(size_t i) { ++fake_transform_ctl[i].close_calls; }

/* One set of entry points per index: match() has no context argument, so a
 * backend's identity is its functions. */
#define FAKE_ENTRY_POINTS(i)                                                                                        \
    static int encoder_match_##i(const xiaotai_video_encoder_config_t *c) { return encoder_match(i, c); }           \
    static xiaotai_err_t encoder_open_##i(const xiaotai_video_encoder_config_t *c, void **s)                        \
    {                                                                                                               \
        return encoder_open(i, c, s);                                                                               \
    }                                                                                                               \
    static void encoder_close_##i(void *s)                                                                          \
    {                                                                                                               \
        (void)s;                                                                                                    \
        encoder_close(i);                                                                                           \
    }                                                                                                               \
    static int decoder_match_##i(const xiaotai_video_decoder_config_t *c) { return decoder_match(i, c); }           \
    static xiaotai_err_t decoder_open_##i(const xiaotai_video_decoder_config_t *c, void **s)                        \
    {                                                                                                               \
        return decoder_open(i, c, s);                                                                               \
    }                                                                                                               \
    static void decoder_close_##i(void *s) { decoder_close(i, s); }                                                 \
    static int transform_match_##i(const xiaotai_video_transform_config_t *c) { return transform_match(i, c); }     \
    static bool transform_limits_##i(xiaotai_video_pixel_t a, xiaotai_video_pixel_t b,                              \
                                     xiaotai_video_scale_limits_t *l)                                               \
    {                                                                                                               \
        return transform_limits(i, a, b, l);                                                                        \
    }                                                                                                               \
    static xiaotai_err_t transform_open_##i(const xiaotai_video_transform_config_t *c, void **s)                    \
    {                                                                                                               \
        return transform_open(i, c, s);                                                                             \
    }                                                                                                               \
    static void transform_close_##i(void *s)                                                                        \
    {                                                                                                               \
        (void)s;                                                                                                    \
        transform_close(i);                                                                                         \
    }

FAKE_ENTRY_POINTS(0)
FAKE_ENTRY_POINTS(1)
FAKE_ENTRY_POINTS(2)
FAKE_ENTRY_POINTS(3)
FAKE_ENTRY_POINTS(4)
FAKE_ENTRY_POINTS(5)
FAKE_ENTRY_POINTS(6)
FAKE_ENTRY_POINTS(7)
FAKE_ENTRY_POINTS(8)
FAKE_ENTRY_POINTS(9)
FAKE_ENTRY_POINTS(10)
FAKE_ENTRY_POINTS(11)

typedef struct {
    int (*encoder_match)(const xiaotai_video_encoder_config_t *);
    xiaotai_err_t (*encoder_open)(const xiaotai_video_encoder_config_t *, void **);
    void (*encoder_close)(void *);
    int (*decoder_match)(const xiaotai_video_decoder_config_t *);
    xiaotai_err_t (*decoder_open)(const xiaotai_video_decoder_config_t *, void **);
    void (*decoder_close)(void *);
    int (*transform_match)(const xiaotai_video_transform_config_t *);
    bool (*transform_limits)(xiaotai_video_pixel_t, xiaotai_video_pixel_t, xiaotai_video_scale_limits_t *);
    xiaotai_err_t (*transform_open)(const xiaotai_video_transform_config_t *, void **);
    void (*transform_close)(void *);
} fake_entry_points_t;

#define FAKE_ENTRY(i)                                                                                                \
    {encoder_match_##i,   encoder_open_##i,     encoder_close_##i,  decoder_match_##i,  decoder_open_##i,            \
     decoder_close_##i,   transform_match_##i,  transform_limits_##i, transform_open_##i, transform_close_##i}

static const fake_entry_points_t k_entries[FAKE_BACKENDS] = {
    FAKE_ENTRY(0), FAKE_ENTRY(1), FAKE_ENTRY(2), FAKE_ENTRY(3), FAKE_ENTRY(4),  FAKE_ENTRY(5),
    FAKE_ENTRY(6), FAKE_ENTRY(7), FAKE_ENTRY(8), FAKE_ENTRY(9), FAKE_ENTRY(10), FAKE_ENTRY(11),
};

static const char *const k_encoder_names[FAKE_BACKENDS] = {"enc0", "enc1", "enc2", "enc3", "enc4",  "enc5",
                                                           "enc6", "enc7", "enc8", "enc9", "enc10", "enc11"};
static const char *const k_decoder_names[FAKE_BACKENDS] = {"dec0", "dec1", "dec2", "dec3", "dec4",  "dec5",
                                                           "dec6", "dec7", "dec8", "dec9", "dec10", "dec11"};
static const char *const k_transform_names[FAKE_BACKENDS] = {"tf0", "tf1", "tf2", "tf3", "tf4",  "tf5",
                                                             "tf6", "tf7", "tf8", "tf9", "tf10", "tf11"};

void fake_backends_reset(void)
{
    memset(fake_encoder_ctl, 0, sizeof(fake_encoder_ctl));
    memset(fake_decoder_ctl, 0, sizeof(fake_decoder_ctl));
    memset(fake_transform_ctl, 0, sizeof(fake_transform_ctl));
    for (size_t i = 0; i < FAKE_BACKENDS; ++i) {
        const fake_entry_points_t *e = &k_entries[i];
        fake_encoders[i] = (xiaotai_video_encoder_t){
            .name = k_encoder_names[i],
            .load = XIAOTAI_VIDEO_LOAD_SOFTWARE,
            .output_alignment = 0,
            .internal_bytes = NULL,
            .match = e->encoder_match,
            .open = e->encoder_open,
            .reconfigure = NULL,
            .encode = encoder_encode,
            .close = e->encoder_close,
        };
        fake_decoders[i] = (xiaotai_video_decoder_t){
            .name = k_decoder_names[i],
            .load = XIAOTAI_VIDEO_LOAD_SOFTWARE,
            .flags = 0,
            .output = XIAOTAI_VIDEO_PIXEL_I420,
            .output_alignment = 0,
            .stack_bytes = 0,
            .stack_internal = false,
            .match = e->decoder_match,
            .open = e->decoder_open,
            .decode = decoder_decode,
            .close = e->decoder_close,
        };
        fake_transforms[i] = (xiaotai_video_transform_t){
            .name = k_transform_names[i],
            .load = XIAOTAI_VIDEO_LOAD_HARDWARE,
            .output_alignment = 0,
            .match = e->transform_match,
            .limits = e->transform_limits,
            .open = e->transform_open,
            .run = transform_run,
            .close = e->transform_close,
        };
    }
}

/* ==================================================================== link */

fake_link_state_t fake_link_state;

static bool link_ready(void *ctx, uint32_t generation)
{
    fake_link_state_t *s = ctx;
    ++s->ready_calls;
    s->last_generation = generation;
    return s->ready;
}

static size_t link_backlog(void *ctx, uint32_t generation)
{
    fake_link_state_t *s = ctx;
    s->last_generation = generation;
    return s->backlog;
}

static xiaotai_err_t link_send(void *ctx, uint32_t generation, const xiaotai_video_packet_t *packet,
                               uint32_t backlog_limit_bytes)
{
    (void)backlog_limit_bytes;
    fake_link_state_t *s = ctx;
    ++s->send_calls;
    s->last_generation = generation;
    s->last_packet = *packet;
    s->last_packet.data = NULL;
    return s->send_result;
}

static bool link_take_key_frame_request(void *ctx, uint32_t generation)
{
    (void)ctx;
    (void)generation;
    return false;
}

static xiaotai_err_t link_request_key_frame(void *ctx, uint32_t generation)
{
    fake_link_state_t *s = ctx;
    ++s->key_requests;
    s->last_generation = generation;
    return XIAOTAI_OK;
}

static xiaotai_err_t link_set_bitrate_range(void *ctx, uint32_t generation, uint32_t min_bps, uint32_t start_bps,
                                            uint32_t max_bps)
{
    (void)ctx;
    (void)generation;
    (void)min_bps;
    (void)start_bps;
    (void)max_bps;
    return XIAOTAI_OK;
}

static bool link_take_bitrate_target(void *ctx, uint32_t generation, uint32_t *target_bps)
{
    (void)ctx;
    (void)generation;
    (void)target_bps;
    return false;
}

const xiaotai_video_link_t *fake_link(void)
{
    static const xiaotai_video_link_t link = {
        .ready = link_ready,
        .backlog = link_backlog,
        .send = link_send,
        .take_key_frame_request = link_take_key_frame_request,
        .request_key_frame = link_request_key_frame,
        .set_bitrate_range = link_set_bitrate_range,
        .take_bitrate_target = link_take_bitrate_target,
        .ctx = &fake_link_state,
    };
    return &link;
}

/* ======================================================= port recorders */

/* Linked with -Wl,--wrap=<name>: every call from another object file reaches
 * __wrap_<name>, which records it and calls the port's __real_<name>. */
void *__real_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size);
void *__real_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size);
void *__real_xiaotai_mem_aligned(xiaotai_mem_kind_t kind, size_t alignment, size_t size);
xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);
int64_t __real_xiaotai_time_us(void);
void *__wrap_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size);
void *__wrap_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size);
void *__wrap_xiaotai_mem_aligned(xiaotai_mem_kind_t kind, size_t alignment, size_t size);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);
int64_t __wrap_xiaotai_time_us(void);

static atomic_uint s_allocations[3];
static fake_task_record_t s_tasks[FAKE_TASK_RECORDS];
static unsigned s_task_count;
static unsigned s_task_fail_countdown;
static xiaotai_err_t s_task_fail_error;
static atomic_bool s_clock_held;
static _Atomic int64_t s_clock_us;

static void count_allocation(xiaotai_mem_kind_t kind)
{
    if ((unsigned)kind < 3U) atomic_fetch_add(&s_allocations[kind], 1U);
}

void *__wrap_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size)
{
    count_allocation(kind);
    return __real_xiaotai_mem_alloc(kind, size);
}

void *__wrap_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size)
{
    count_allocation(kind);
    return __real_xiaotai_mem_calloc(kind, count, size);
}

void *__wrap_xiaotai_mem_aligned(xiaotai_mem_kind_t kind, size_t alignment, size_t size)
{
    count_allocation(kind);
    return __real_xiaotai_mem_aligned(kind, alignment, size);
}

xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task)
{
    if (s_task_count < FAKE_TASK_RECORDS && config) {
        fake_task_record_t *record = &s_tasks[s_task_count];
        record->config = *config;
        snprintf(record->name, sizeof(record->name), "%s", config->name ? config->name : "");
        record->config.name = record->name;
    }
    ++s_task_count;
    if (s_task_fail_countdown && --s_task_fail_countdown == 0U) return s_task_fail_error;
    return __real_xiaotai_task_create(config, task);
}

int64_t __wrap_xiaotai_time_us(void)
{
    return atomic_load(&s_clock_held) ? atomic_load(&s_clock_us) : __real_xiaotai_time_us();
}

void fake_port_clear(void)
{
    for (size_t i = 0; i < 3U; ++i) atomic_store(&s_allocations[i], 0U);
    memset(s_tasks, 0, sizeof(s_tasks));
    s_task_count = 0;
    s_task_fail_countdown = 0;
}

unsigned fake_mem_allocations(xiaotai_mem_kind_t kind)
{
    return (unsigned)kind < 3U ? atomic_load(&s_allocations[kind]) : 0U;
}

unsigned fake_task_count(void) { return s_task_count; }

const fake_task_record_t *fake_task_at(size_t index)
{
    return index < s_task_count && index < FAKE_TASK_RECORDS ? &s_tasks[index] : NULL;
}

const fake_task_record_t *fake_task_named(const char *name)
{
    for (size_t i = 0; i < s_task_count && i < FAKE_TASK_RECORDS; ++i) {
        if (strcmp(s_tasks[i].name, name) == 0) return &s_tasks[i];
    }
    return NULL;
}

void fake_task_fail_nth(unsigned n, xiaotai_err_t error)
{
    s_task_fail_countdown = n;
    s_task_fail_error = error;
}

void fake_clock_hold(int64_t us)
{
    atomic_store(&s_clock_us, us);
    atomic_store(&s_clock_held, true);
}

void fake_clock_advance_us(int64_t us) { atomic_fetch_add(&s_clock_us, us); }

void fake_clock_release(void) { atomic_store(&s_clock_held, false); }
