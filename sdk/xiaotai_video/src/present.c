// SPDX-License-Identifier: MIT
#include "present.h"

#include "convert.h"
#include "geometry.h"
#include "paint.h"
#include "playout.h"

enum { CACHE_ALIGNMENT = 64, LEGACY_INDEPENDENT_FPS = 12 };

typedef enum { FREE, WRITING, READY, PINNED } slot_state_t;

typedef struct {
    uint8_t *pixels;
    size_t capacity;
    xiaotai_video_frame_t frame;
    uint64_t cookie, order, epoch;
    slot_state_t state;
} slot_t;

typedef struct {
    unsigned slots;
    uint8_t scale;
    uint16_t width, height;
    xiaotai_video_pixel_t pixel;
    size_t bytes;
    bool adaptive;
} layout_t;

/* The session task alone gives a slot its pixels and takes them back. */

static struct {
    atomic_bool initialized;
    xiaotai_critical_t lock;
    slot_t *slots;
    unsigned slot_count;
    size_t alignment;
    int pinned;
    uint32_t revision, generation, sequence;
    uint64_t next_cookie, next_order, epoch;
    bool has_presented;
    layout_t layout;
    video_playout_t playout;
} pool = {.pinned = -1};

static void lock(void) { xiaotai_critical_enter(&pool.lock); }
static void unlock(void) { xiaotai_critical_exit(&pool.lock); }

static bool adaptive(const xiaotai_video_downlink_t *d)
{
    if (d->codec == XIAOTAI_VIDEO_CODEC_JPEG) return video_engine.buffer_independent_frames;
    return d->codec == XIAOTAI_VIDEO_CODEC_H264 && d->burst_ms != 0;
}

uint8_t video_present_render_scale(const xiaotai_video_viewport_t *viewport,
                                   const xiaotai_video_downlink_t *downlink)
{
    if (!viewport || !downlink || !viewport->width || !viewport->height) return 0;
    if (downlink->codec == XIAOTAI_VIDEO_CODEC_JPEG)
        return video_geometry_render_scale(viewport->width, viewport->height, downlink->max_width, downlink->max_height);
    for (uint8_t n = 4; n > 1U; --n) {
        if (viewport->width % n == 0 && viewport->height % n == 0 &&
            viewport->width / n >= downlink->max_width && viewport->height / n >= downlink->max_height) return n;
    }
    return 1;
}

static layout_t layout(const video_viewport_entry_t *entry, const xiaotai_video_downlink_t *downlink)
{
    const xiaotai_video_viewport_t *viewport = &entry->viewport;
    const uint8_t scale = video_present_render_scale(viewport, downlink);
    const bool buffered = adaptive(downlink);
    const unsigned defaults = buffered ? VIDEO_VIEWPORT_BUFFERED_FRAMES : VIDEO_VIEWPORT_PLAIN_FRAMES;
    unsigned slots = entry->default_frames && viewport->frames < defaults ? defaults : viewport->frames;
    const unsigned limit = video_engine.downlink_pictures;
    if (limit && slots > limit) slots = limit;
    if (!scale || !video_geometry_rgb_bytes(viewport->pixel) ||
        slots < (limit ? VIDEO_VIEWPORT_LIMITED_FRAMES : VIDEO_VIEWPORT_MIN_FRAMES)) return (layout_t){0};
    const uint16_t width = viewport->width / scale, height = viewport->height / scale;
    return (layout_t){.slots = slots, .scale = scale, .width = width, .height = height, .pixel = viewport->pixel,
                      .bytes = video_geometry_image_bytes(viewport->pixel, width, height), .adaptive = buffered};
}

static void free_slots(slot_t *slots, unsigned count)
{
    if (slots) for (unsigned i = 0; i < count; ++i) xiaotai_mem_free(slots[i].pixels);
    xiaotai_mem_free(slots);
}

static xiaotai_err_t plan_slot_capacities(slot_t *slots, unsigned *count)
{
    const video_requirements_t *req = &video_engine.requirements;
    for (unsigned i = 0; i < VIDEO_DOWNLINK_CODECS; ++i) {
        const xiaotai_video_downlink_t *d = &req->downlinks[i];
        if (!d->codec || !xiaotai_video_decoder_find(&(const xiaotai_video_decoder_config_t){
                .codec = d->codec, .max_width = d->max_width, .max_height = d->max_height})) continue;
        for (size_t j = 0; j < req->viewport_count; ++j) {
            const layout_t pair = layout(&req->viewports[j], d);
            if (!pair.bytes) return XIAOTAI_ERR_INVALID_SIZE;
            if (!slots && pair.slots > *count) *count = pair.slots;
            if (slots) {
                if (pair.slots > *count) return XIAOTAI_ERR_INVALID_SIZE;
                for (unsigned n = 0; n < pair.slots; ++n)
                    if (slots[n].capacity < pair.bytes) slots[n].capacity = pair.bytes;
            }
        }
    }
    return *count ? XIAOTAI_OK : XIAOTAI_ERR_NOT_SUPPORTED;
}

xiaotai_err_t video_present_prepare(void)
{
    /* start/reset are serialized by the engine owner; runtime calls may also
     * arrive before startup or after reset, so the lock itself stays alive. */
    if (!atomic_load(&pool.initialized)) {
        xiaotai_critical_init(&pool.lock);
        atomic_store(&pool.initialized, true);
    }
    lock();
    const bool prepared = pool.slots != NULL;
    unlock();
    if (prepared) return XIAOTAI_OK;

    unsigned count = 0;
    xiaotai_err_t result = plan_slot_capacities(NULL, &count);
    if (result != XIAOTAI_OK) return result;

    size_t alignment = CACHE_ALIGNMENT;
    for (size_t i = 0; i < video_registry_count(VIDEO_BACKEND_TRANSFORM); ++i) {
        const size_t requested = video_transform_at(i)->output_alignment;
        if (requested && (requested & (requested - 1U))) return XIAOTAI_ERR_INVALID_SIZE;
        if (requested > alignment) alignment = requested;
    }
    slot_t *slots = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, count, sizeof(*slots));
    if (!slots) return XIAOTAI_ERR_NO_MEM;
    result = plan_slot_capacities(slots, &count);
    if (result != XIAOTAI_OK) { free_slots(slots, count); return result; }
    for (unsigned i = 0; i < count; ++i) {
        if (!slots[i].capacity || slots[i].capacity > SIZE_MAX - (alignment - 1U)) {
            free_slots(slots, count); return XIAOTAI_ERR_INVALID_SIZE;
        }
        slots[i].capacity = (slots[i].capacity + alignment - 1U) & ~(alignment - 1U);
        if (video_engine.downlink_on_demand) continue;
        slots[i].pixels = xiaotai_mem_aligned(XIAOTAI_MEM_BULK, alignment, slots[i].capacity);
        if (!slots[i].pixels) { free_slots(slots, count); return XIAOTAI_ERR_NO_MEM; }
    }
    lock();
    pool.slots = slots;
    pool.slot_count = count;
    pool.alignment = alignment;
    unlock();
    return XIAOTAI_OK;
}

size_t video_present_missing(const video_session_config_t *config, size_t *largest)
{
    const video_viewport_entry_t *entry = video_require_viewport_for(config->display);
    if (!entry || !pool.slots) return 0;
    const layout_t next = layout(entry, &config->downlink);
    size_t total = 0;
    for (unsigned i = 0; i < next.slots && i < pool.slot_count; ++i) {
        if (pool.slots[i].pixels) continue;
        total += pool.slots[i].capacity;
        if (pool.slots[i].capacity > *largest) *largest = pool.slots[i].capacity;
    }
    return total;
}

xiaotai_err_t video_present_take(const video_session_config_t *config)
{
    const video_viewport_entry_t *entry = video_require_viewport_for(config->display);
    if (!entry || !pool.slots) return XIAOTAI_ERR_NOT_SUPPORTED;
    const layout_t next = layout(entry, &config->downlink);
    for (unsigned i = 0; i < next.slots && i < pool.slot_count; ++i) {
        if (pool.slots[i].pixels) continue;
        uint8_t *pixels = xiaotai_mem_aligned(XIAOTAI_MEM_BULK, pool.alignment, pool.slots[i].capacity);
        if (!pixels) return XIAOTAI_ERR_NO_MEM;
        lock();
        pool.slots[i].pixels = pixels;
        unlock();
    }
    return XIAOTAI_OK;
}

bool video_present_release_idle(void)
{
    if (!pool.slots) return true;
    lock();
    bool idle = !pool.revision;
    for (unsigned i = 0; i < pool.slot_count; ++i)
        idle &= pool.slots[i].state != WRITING && pool.slots[i].state != PINNED;
    unlock();
    /* Without a revision no slot is claimed and none becomes ready. */
    for (unsigned i = 0; idle && i < pool.slot_count; ++i) {
        lock();
        uint8_t *pixels = pool.slots[i].pixels;
        pool.slots[i].pixels = NULL;
        pool.slots[i].state = FREE;
        unlock();
        xiaotai_mem_free(pixels);
    }
    return idle;
}

static void clear_ready(void)
{
    /* An in-flight conversion keeps its byte range until it releases it, but
     * it cannot publish into a different reference chain. */
    ++pool.epoch;
    for (unsigned i = 0; i < pool.layout.slots; ++i)
        if (pool.slots[i].state == READY) pool.slots[i].state = FREE;
}

bool video_present_reset(void)
{
    if (!atomic_load(&pool.initialized)) return true;
    lock();
    clear_ready();
    pool.revision = 0;
    pool.layout.slots = 0;
    for (unsigned i = 0; i < pool.slot_count; ++i) {
        const slot_state_t state = pool.slots[i].state;
        if (state == PINNED || state == WRITING) { unlock(); return false; }
    }
    slot_t *slots = pool.slots;
    const unsigned count = pool.slot_count;
    pool.slots = NULL;
    pool.slot_count = 0;
    unlock();
    free_slots(slots, count);
    return true;
}

xiaotai_err_t video_present_begin(const video_session_config_t *config)
{
    if (!config || !config->generation || !config->revision || !config->has_downlink) return XIAOTAI_ERR_INVALID_ARG;
    if (!atomic_load(&pool.initialized)) return XIAOTAI_ERR_INVALID_STATE;
    const video_viewport_entry_t *entry = video_require_viewport_for(config->display);
    if (!entry) return XIAOTAI_ERR_NOT_SUPPORTED;
    const layout_t next = layout(entry, &config->downlink);
    lock();
    if (pool.revision) { unlock(); return XIAOTAI_ERR_BUSY; }
    if (!next.bytes || next.slots > pool.slot_count || pool.slots[next.slots - 1U].capacity < next.bytes) {
        unlock(); return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    pool.layout = next;
    pool.revision = config->revision;
    pool.generation = config->generation;
    pool.sequence = 0;
    pool.has_presented = false;
    ++pool.epoch;
    const uint8_t fps = config->downlink.fps ? config->downlink.fps :
                        (next.adaptive ? LEGACY_INDEPENDENT_FPS : 0U);
    video_playout_reset(&pool.playout, fps, next.adaptive, next.slots - 1U);
    unlock();
    return XIAOTAI_OK;
}

void video_present_stop(uint32_t revision)
{
    if (!revision || !atomic_load(&pool.initialized)) return;
    lock();
    if (revision == pool.revision) {
        clear_ready();
        pool.revision = 0;
        pool.layout.slots = 0;
    }
    unlock();
}

void video_present_flush(uint32_t revision)
{
    if (!revision || !atomic_load(&pool.initialized)) return;
    lock();
    if (revision == pool.revision) clear_ready();
    unlock();
}

void video_present_arrival(uint32_t revision, int64_t arrival_us)
{
    if (!revision || !atomic_load(&pool.initialized)) return;
    lock();
    if (revision == pool.revision) (void)video_playout_arrival(&pool.playout, arrival_us);
    unlock();
}

bool video_present_buffering(uint32_t revision, int64_t now_us)
{
    if (!revision || !atomic_load(&pool.initialized)) return false;
    lock();
    const bool buffering = revision == pool.revision && video_playout_buffering(&pool.playout, now_us);
    unlock();
    return buffering;
}

/* Under the lock. */
static bool writable(const slot_t *slot)
{
    return slot->state == FREE || (slot->state == READY && pool.layout.slots >= VIDEO_VIEWPORT_MIN_FRAMES);
}

bool video_present_writable(void)
{
    if (!atomic_load(&pool.initialized)) return true;
    lock();
    bool found = !pool.revision;    /* the claim says why not */
    for (unsigned i = 0; i < pool.layout.slots && !found; ++i) found = writable(&pool.slots[i]);
    unlock();
    return found;
}

bool video_present_claim(uint32_t revision, size_t required, video_present_write_t *out)
{
    if (!revision || !required || !out || !atomic_load(&pool.initialized)) return false;
    lock();
    int chosen = -1;
    if (revision == pool.revision && required >= pool.layout.bytes) {
        /* Prefer the smallest useful FREE slot to avoid needlessly occupying
         * the larger capacities shared by other viewport/codec pairs. */
        for (unsigned i = 0; i < pool.layout.slots; ++i) {
            const slot_t *slot = &pool.slots[i];
            if (slot->capacity < required || !writable(slot)) continue;
            if (chosen < 0 || (slot->state == FREE && (pool.slots[chosen].state != FREE ||
                slot->capacity < pool.slots[chosen].capacity)) ||
                (slot->state == READY && pool.slots[chosen].state == READY && slot->order < pool.slots[chosen].order))
                chosen = (int)i;
        }
    }
    if (chosen >= 0) {
        slot_t *slot = &pool.slots[chosen];
        slot->state = WRITING;
        slot->epoch = pool.epoch;
        if (++pool.next_cookie == 0U) ++pool.next_cookie;
        slot->cookie = pool.next_cookie;
        *out = (video_present_write_t){.pixels = slot->pixels, .capacity = slot->capacity,
                                      .cookie = slot->cookie, .slot = (uint8_t)chosen};
    }
    unlock();
    return chosen >= 0;
}

static bool finish(video_present_write_t *write, const xiaotai_video_frame_t *frame)
{
    if (!write || !write->cookie || !atomic_load(&pool.initialized)) return false;
    lock();
    bool published = false;
    if (write->slot < pool.slot_count) {
        slot_t *slot = &pool.slots[write->slot];
        if (slot->state == WRITING && slot->cookie == write->cookie && slot->pixels == write->pixels) {
            const layout_t *l = &pool.layout;
            if (frame && slot->epoch == pool.epoch && pool.revision && frame->generation == pool.generation &&
                frame->pixels == slot->pixels && frame->size == l->bytes && frame->size <= slot->capacity &&
                frame->pixel == l->pixel && frame->width == l->width && frame->height == l->height &&
                frame->scale == l->scale && frame->picture.width && frame->picture.height &&
                (uint32_t)frame->picture.x + frame->picture.width <= frame->width &&
                (uint32_t)frame->picture.y + frame->picture.height <= frame->height) {
                slot->frame = *frame;
                slot->order = ++pool.next_order;
                published = true;
            }
            slot->state = published ? READY : FREE;
        }
    }
    unlock();
    *write = (video_present_write_t){0};
    return published;
}

bool video_present_publish(video_present_write_t *write, const xiaotai_video_frame_t *frame)
{
    return finish(write, frame);
}

void video_present_cancel(video_present_write_t *write)
{
    (void)finish(write, NULL);
}

static bool acquire(uint32_t generation, xiaotai_video_frame_t *frame)
{
    if (!atomic_load(&pool.initialized)) return false;
    const int64_t now = xiaotai_time_us();
    lock();
    if (generation && (generation != pool.generation || !video_session_downlink_current(pool.revision))) {
        unlock();
        return false;
    }
    int latest = -1, oldest = -1;
    size_t ready = 0;
    for (unsigned i = 0; i < pool.layout.slots; ++i) {
        const slot_t *slot = &pool.slots[i];
        if (slot->state != READY) continue;
        if (latest < 0 || slot->order > pool.slots[latest].order) latest = (int)i;
        if (oldest < 0 || slot->order < pool.slots[oldest].order) oldest = (int)i;
        ++ready;
    }
    const video_playout_choice_t choice = video_playout_select(&pool.playout, now, ready, pool.has_presented);
    if (choice == VIDEO_PLAYOUT_HOLD) { unlock(); return false; }
    const int selected = choice == VIDEO_PLAYOUT_LATEST ? latest : oldest;
    if (selected < 0) { unlock(); return false; }
    slot_t *next = &pool.slots[selected];
    next->frame.sequence = ++pool.sequence;
    next->state = PINNED;
    video_paint_pin(&next->frame);
    if (pool.pinned >= 0) pool.slots[pool.pinned].state = FREE;
    pool.pinned = selected;
    pool.has_presented = true;
    if (choice == VIDEO_PLAYOUT_LATEST) {
        for (unsigned i = 0; i < pool.layout.slots; ++i)
            if (pool.slots[i].state == READY) pool.slots[i].state = FREE;
    }
    if (frame) *frame = next->frame;
    const uint32_t revision = pool.revision;
    unlock();
    video_session_downlink_presented(revision);
    video_convert_kick(false);      /* a slot is free again */
    return true;
}

bool xiaotai_video_frame_acquire(xiaotai_video_frame_t *frame) { return acquire(0, frame); }

bool xiaotai_video_frame_acquire_for(uint32_t generation, xiaotai_video_frame_t *frame)
{
    return generation && acquire(generation, frame);
}

void xiaotai_video_frame_release(void)
{
    if (!atomic_load(&pool.initialized)) return;
    lock();
    if (pool.pinned >= 0) {
        video_paint_pin(NULL);
        pool.slots[pool.pinned].state = FREE;
        pool.pinned = -1;
    }
    unlock();
}
