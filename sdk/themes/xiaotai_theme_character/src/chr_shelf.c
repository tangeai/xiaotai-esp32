// SPDX-License-Identifier: MIT
#include "chr_shelf.h"

#include <stdatomic.h>
#include <string.h>

#include "chr_sprites.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

#define TAG "character"

enum {
    PAGE = 0x10000,         /* where what follows an image may begin */
    PLACES = 4,             /* the picture shown and the one it is a patch over, the next one, one to spare */
    TRIES = 3,              /* a picture that could not be read this often is not asked for again */
    STACK = 4096, PRIORITY = 4,
};

/* A place belongs to the step while it is FREE or READY, to the task while it is LOADING. */
enum { FREE = 0, LOADING, READY };

typedef struct {
    uint8_t *bytes;
    uint16_t id;
    uint32_t used;
    atomic_int state;
} place_t;

struct chr_shelves {
    chr_medium_t medium;
    chr_shelf_t shelf;
    chr_pack_t pack;
    uint32_t at;                                /* where the pack begins on the medium */
    uint8_t *head, *resident;
    _Atomic(const uint8_t *) *where;            /* per sprite: its bytes, NULL while they are not in memory */
    atomic_uchar *tries;                        /* per sprite, written by the task */
    uint16_t count;
    place_t place[PLACES];
    unsigned places;
    uint32_t place_bytes, resident_bytes, clock;
    xiaotai_task_t task;
    xiaotai_signal_t opened, wake, done;
    bool open, usable, quit;
    atomic_uint loads, load_max_ms, failed, open_ms, stack_free;
};

uint32_t chr_image_end(const chr_medium_t *m)
{
    uint8_t head[24], segment[8];
    if (m->size < sizeof(head) || !m->read(m->ctx, 0, head, sizeof(head)) || head[0] != 0xE9) return 0;
    uint64_t at = sizeof(head);
    for (unsigned i = 0; i < head[1]; ++i) {
        uint32_t length;
        if (at + sizeof(segment) > m->size || !m->read(m->ctx, (uint32_t)at, segment, sizeof(segment))) return 0;
        memcpy(&length, segment + 4, sizeof(length));
        at += sizeof(segment) + length;
    }
    at = (at + 16U) & ~(uint64_t)15U;           /* padding up to a checksum in the last byte of sixteen */
    if (head[23]) at += 32U;                    /* a hash of it all */
    return at <= m->size ? (uint32_t)at : 0;
}

/* Only ever shown as a pose, or as a picture of a pose's clip: what is read when it is wanted. */
static bool on_demand(const chr_pack_t *pack, unsigned id)
{
    for (unsigned slot = 0; slot < pack->slot_count; ++slot)
        if ((slot < SL_POSE || slot >= SL_POSE + CHR_POSES) && chr_pack_slot(pack, slot) == id) return false;
    for (unsigned slot = SL_POSE; slot < SL_POSE + CHR_POSES; ++slot)
        if (chr_pack_slot(pack, slot) == id) return true;
    chr_frame_t frame;
    for (unsigned i = 0; chr_pack_frame(pack, i, &frame); ++i)
        if (frame.sprite == id || frame.under == id) return true;
    return false;
}

static bool load(chr_shelves_t *s, unsigned id, uint8_t *to)
{
    uint32_t start, length;
    return chr_pack_place(&s->pack, id, &start, &length) && s->medium.read(s->medium.ctx, s->at + start, to, length) &&
           chr_pack_intact(&s->pack, id, to);
}

/* Finds the pack, and brings in what stays in memory. Allocates everything there will ever be. */
static bool open_pack(chr_shelves_t *s)
{
    uint8_t fixed[CHR_PACK_FIXED];
    const uint32_t end = chr_image_end(&s->medium);
    if (!end) return false;
    s->at = (end + PAGE - 1U) / PAGE * PAGE;
    if (s->at > s->medium.size || s->medium.size - s->at < sizeof(fixed) || !s->medium.read(s->medium.ctx, s->at, fixed, sizeof(fixed)))
        return false;
    uint32_t total = 0;
    const uint32_t head = chr_pack_head(fixed, &total);
    if (!head || total > s->medium.size - s->at) return false;
    s->head = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, head);
    if (!s->head || !s->medium.read(s->medium.ctx, s->at, s->head, head) ||
        !chr_pack_open_head(&s->pack, s->head, head, &s->shelf)) return false;

    s->count = s->pack.count;
    s->where = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, s->count, sizeof(*s->where));
    s->tries = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, s->count, sizeof(*s->tries));
    if (!s->where || !s->tries) return false;
    uint32_t start, length;
    unsigned poses = 0;
    for (unsigned id = 0; id < s->count; ++id) {
        atomic_init(&s->where[id], NULL);
        if (!chr_pack_place(&s->pack, id, &start, &length)) return false;
        length = (length + 3U) & ~3U;
        if (!on_demand(&s->pack, id)) s->resident_bytes += length;
        else {
            ++poses;
            if (length > s->place_bytes) s->place_bytes = length;
        }
    }
    s->resident = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, s->resident_bytes ? s->resident_bytes : 4U);
    if (!s->resident) return false;
    uint32_t used = 0;
    for (unsigned id = 0; id < s->count; ++id) {
        if (on_demand(&s->pack, id) || !chr_pack_place(&s->pack, id, &start, &length)) continue;
        if (!load(s, id, s->resident + used)) return false;
        atomic_store_explicit(&s->where[id], s->resident + used, memory_order_release);
        used += (length + 3U) & ~3U;
    }
    /* Without room for a single pose she keeps to her base picture. */
    for (s->places = 0; s->places < PLACES && s->places < poses; ++s->places) {
        place_t *p = &s->place[s->places];
        p->bytes = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, s->place_bytes);
        if (!p->bytes) break;
        p->id = CHR_NO_SPRITE;
        atomic_init(&p->state, FREE);
    }
    return true;
}

static void serve(chr_shelves_t *s, place_t *p)
{
    const int64_t began = xiaotai_time_us();
    const bool ok = load(s, p->id, p->bytes);
    const unsigned took = (unsigned)((xiaotai_time_us() - began) / 1000);
    if (took > atomic_load_explicit(&s->load_max_ms, memory_order_relaxed))
        atomic_store_explicit(&s->load_max_ms, took, memory_order_relaxed);
    if (ok) {
        atomic_store_explicit(&s->tries[p->id], 0, memory_order_relaxed);
        atomic_store_explicit(&s->where[p->id], p->bytes, memory_order_release);
        atomic_fetch_add_explicit(&s->loads, 1U, memory_order_relaxed);
        atomic_store_explicit(&p->state, READY, memory_order_release);
        return;
    }
    const unsigned tried = atomic_load_explicit(&s->tries[p->id], memory_order_relaxed) + 1U;
    atomic_store_explicit(&s->tries[p->id], (unsigned char)tried, memory_order_relaxed);
    if (tried == TRIES) XIAOTAI_LOGW(TAG, "picture %u cannot be read", (unsigned)p->id);
    atomic_fetch_add_explicit(&s->failed, 1U, memory_order_relaxed);
    atomic_store_explicit(&p->state, FREE, memory_order_release);
}

static void run(void *arg)
{
    chr_shelves_t *s = arg;
    const int64_t began = xiaotai_time_us();
    s->medium.size = s->medium.open(s->medium.ctx);
    s->usable = s->medium.size && open_pack(s);
    atomic_store_explicit(&s->open_ms, (unsigned)((xiaotai_time_us() - began) / 1000), memory_order_relaxed);
    atomic_store_explicit(&s->stack_free, xiaotai_task_current_stack_free(), memory_order_relaxed);
    xiaotai_signal_give(&s->opened);
    while (s->usable) {
        (void)xiaotai_signal_take(&s->wake, XIAOTAI_WAIT_FOREVER);
        if (s->quit) break;
        for (unsigned i = 0; i < s->places; ++i)
            if (atomic_load_explicit(&s->place[i].state, memory_order_acquire) == LOADING) serve(s, &s->place[i]);
        atomic_store_explicit(&s->stack_free, xiaotai_task_current_stack_free(), memory_order_relaxed);
    }
    xiaotai_signal_give(&s->done);
    xiaotai_task_exit();
}

static const uint8_t *find(void *ctx, unsigned id)
{
    const chr_shelves_t *s = ctx;
    return id < s->count ? atomic_load_explicit(&s->where[id], memory_order_acquire) : NULL;
}

static bool among(const uint16_t *ids, unsigned count, uint16_t id)
{
    for (unsigned i = 0; i < count; ++i)
        if (ids[i] == id) return true;
    return false;
}

static chr_shelf_state_t want(void *ctx, const uint16_t *ids, unsigned count, const uint16_t *kept, unsigned kept_count)
{
    chr_shelves_t *s = ctx;
    chr_shelf_state_t all = CHR_SHELF_READY;
    bool posted = false;
    ++s->clock;
    for (unsigned i = 0; i < count; ++i) {
        const uint16_t id = ids[i];
        if (id == CHR_NO_SPRITE) continue;
        if (id >= s->count) return CHR_SHELF_FAILED;
        place_t *held = NULL, *victim = NULL;
        for (unsigned k = 0; k < s->places && !held; ++k)
            if (s->place[k].id == id) held = &s->place[k];
        if (held) held->used = s->clock;
        if (find(s, id)) continue;                                  /* here, or one that always is */
        if (held && atomic_load_explicit(&held->state, memory_order_acquire) == LOADING) {
            if (all == CHR_SHELF_READY) all = CHR_SHELF_COMING;
            continue;
        }
        if (find(s, id)) continue;                                  /* it came while we looked */
        if (!on_demand(&s->pack, id) || !s->places || atomic_load_explicit(&s->tries[id], memory_order_relaxed) >= TRIES) {
            all = CHR_SHELF_FAILED;
            continue;
        }
        /* The place nobody needs that was needed longest ago. */
        for (unsigned k = 0; k < s->places; ++k) {
            place_t *p = &s->place[k];
            if (atomic_load_explicit(&p->state, memory_order_acquire) == LOADING) continue;
            if (p != held && p->id != CHR_NO_SPRITE && (among(kept, kept_count, p->id) || among(ids, count, p->id))) continue;
            if (p == held) { victim = p; break; }
            if (!victim || p->used < victim->used) victim = p;
        }
        if (all == CHR_SHELF_READY) all = CHR_SHELF_COMING;
        if (!victim) continue;                                      /* all busy: asked for again at the next step */
        if (victim->id != CHR_NO_SPRITE) atomic_store_explicit(&s->where[victim->id], NULL, memory_order_release);
        victim->id = id;
        victim->used = s->clock;
        atomic_store_explicit(&victim->state, LOADING, memory_order_release);
        posted = true;
    }
    if (posted) xiaotai_signal_give(&s->wake);
    return all;
}

static void release(chr_shelves_t *s)
{
    for (unsigned i = 0; i < PLACES; ++i) xiaotai_mem_free(s->place[i].bytes);
    xiaotai_mem_free(s->resident);
    xiaotai_mem_free(s->tries);
    xiaotai_mem_free(s->where);
    xiaotai_mem_free(s->head);
    xiaotai_signal_deinit(&s->done);
    xiaotai_signal_deinit(&s->wake);
    xiaotai_signal_deinit(&s->opened);
    xiaotai_mem_free(s);
}

chr_shelves_t *chr_shelves_open(const chr_medium_t *medium, uint32_t wait_ms)
{
    if (!medium || !medium->open || !medium->read) return NULL;
    chr_shelves_t *s = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*s));
    if (!s) return NULL;
    s->medium = *medium;
    s->shelf = (chr_shelf_t){.find = find, .want = want, .ctx = s};
    if (xiaotai_signal_init(&s->opened) != XIAOTAI_OK) goto no_opened;
    if (xiaotai_signal_init(&s->wake) != XIAOTAI_OK) goto no_wake;
    if (xiaotai_signal_init(&s->done) != XIAOTAI_OK) goto no_done;
    /* Its stack must stay reachable while the flash driver stops the caches. */
    const xiaotai_task_config_t config = {.name = "chr_shelf", .entry = run, .arg = s, .stack_bytes = STACK,
        .stack = XIAOTAI_MEM_INTERNAL, .priority = PRIORITY, .core = XIAOTAI_CORE_ANY};
    if (xiaotai_task_create(&config, &s->task) != XIAOTAI_OK) {
        release(s);
        return NULL;
    }
    s->open = xiaotai_signal_take(&s->opened, wait_ms);
    if (s->open && s->usable) {
        XIAOTAI_LOGI(TAG, "pack at %#x: %u pictures, %u KB, %u KB in memory, %u places of %u KB, opened in %u ms",
                     (unsigned)s->at, (unsigned)s->count, (unsigned)(s->pack.size / 1024U),
                     (unsigned)((s->resident_bytes + s->pack.head) / 1024U), s->places, (unsigned)(s->place_bytes / 1024U),
                     atomic_load_explicit(&s->open_ms, memory_order_relaxed));
        return s;
    }
    XIAOTAI_LOGW(TAG, "no picture pack behind the image");
    chr_shelves_close(s);
    return NULL;
no_done:
    xiaotai_signal_deinit(&s->wake);
no_wake:
    xiaotai_signal_deinit(&s->opened);
no_opened:
    xiaotai_mem_free(s);
    return NULL;
}

const chr_pack_t *chr_shelves_pack(const chr_shelves_t *s) { return s ? &s->pack : NULL; }

void chr_shelves_info(const chr_shelves_t *s, chr_shelves_info_t *out)
{
    if (!s || !out) return;
#define N(field) atomic_load_explicit(&s->field, memory_order_relaxed)
    *out = (chr_shelves_info_t){.at = s->at, .bytes = s->pack.size, .resident = s->resident_bytes + s->pack.head,
        .places = s->places, .place_bytes = s->place_bytes, .open_ms = N(open_ms), .loads = N(loads),
        .load_max_ms = N(load_max_ms), .failed = N(failed), .stack_free = N(stack_free)};
#undef N
}

void chr_shelves_close(chr_shelves_t *s)
{
    if (!s) return;
    /* Opened or not, the task ends by itself or at the next wake; it is waited for. */
    s->quit = true;
    xiaotai_signal_give(&s->wake);
    (void)xiaotai_signal_take(&s->done, XIAOTAI_WAIT_FOREVER);
    release(s);
}
