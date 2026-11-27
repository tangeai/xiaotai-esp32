// SPDX-License-Identifier: MIT
#include "downlink.h"

#include <string.h>
#include "convert.h"
#include "h264_au.h"
#include "xiaotai/eventlog.h"

enum {
    SW_FRAMES = 4, HW_FRAMES = 1,
    WAIT_MS = 50, STALL_MS = 2000,
    LATE_FRAME_INTERVALS = 12, LATE_QUEUE = 12, LATE_SAMPLES = 8,
};

typedef struct {
    xiaotai_video_packet_t packet;
    uint8_t *bytes;
    uint32_t revision, epoch;
    int64_t queued_us;
} compressed_t;

typedef enum { FRAME_FREE, FRAME_WRITING, FRAME_READY, FRAME_LEASED } frame_state_t;

typedef struct {
    uint8_t *bytes;
    size_t capacity;
    frame_state_t state;
    uint64_t order;
    video_downlink_frame_t frame;
} decoded_t;

typedef struct {
    const xiaotai_video_decoder_t *backend;
    void *state;
    uint32_t revision, epoch;
    video_h264_chain_t chain;
    uint8_t late_samples, decoded_in_cycle;
    uint8_t starved;        /* decodes in a row without memory */
    bool drop_to_key;
} worker_t;

static struct {
    xiaotai_critical_t lock;
    bool lock_ready;
    compressed_t *slots;
    unsigned slot_count, queue_count, signal_count, writers, sw_frames;
    size_t slot_bytes;
    xiaotai_queue_t free, incoming, pending;
    xiaotai_signal_t relay_work, hardware_work;
    decoded_t frames[SW_FRAMES + HW_FRAMES];
    uint64_t ready_order;
    /* Session task publishes a revision and the decoder that owns it. Queue
     * entries and converter leases are never recycled while held by a worker. */
    uint32_t revision, generation, epoch, watched_revision, watched_generation, reference;
    xiaotai_video_downlink_t config;
    const xiaotai_video_decoder_t *backend;
    bool require_recovery, busy, quarantined;
    int64_t watched_since;
} rx;

/* When changing quarantine, take rx.lock before the session status lock.
 * The session never calls a receiver while holding its own status lock. */
static void lock(void) { xiaotai_critical_enter(&rx.lock); }
static void unlock(void) { xiaotai_critical_exit(&rx.lock); }

static void returned(uint8_t index)
{
    (void)xiaotai_queue_send(&rx.free, &index, 0);
}

static void drain(xiaotai_queue_t *queue)
{
    uint8_t index;
    while (xiaotai_queue_receive(queue, &index, 0)) returned(index);
}

static void discard_ready(void)
{
    ++rx.reference;
    for (size_t i = 0; i < SW_FRAMES + HW_FRAMES; ++i)
        if (rx.frames[i].state == FRAME_READY) rx.frames[i].state = FRAME_FREE;
    video_present_flush(rx.revision);
}

static unsigned queued(void)
{
    return (unsigned)(xiaotai_queue_count(&rx.incoming) + xiaotai_queue_count(&rx.pending));
}

static void publish(uint32_t revision, unsigned discarded, unsigned rejected, unsigned decoded,
                    unsigned failed, unsigned gaps, const xiaotai_video_image_t *image)
{
    const video_downlink_update_t update = {.discarded = discarded, .rejected = rejected, .decoded = decoded,
        .failed = failed, .gaps = gaps, .depth = queued(), .width = image ? image->width : 0,
        .height = image ? image->height : 0};
    video_session_downlink_update(revision, &update);
}

static const xiaotai_video_decoder_t *decoder(const xiaotai_video_downlink_t *config)
{
    return xiaotai_video_decoder_find(&(const xiaotai_video_decoder_config_t){
        .codec = config->codec, .max_width = config->max_width, .max_height = config->max_height});
}

static bool reserve_decode(const xiaotai_video_downlink_t *config, const xiaotai_video_decoder_t *backend,
                           size_t size[2], size_t alignment[2]);

static uint8_t *frame_memory(size_t size, size_t alignment)
{
    return alignment > 1U ? xiaotai_mem_aligned(XIAOTAI_MEM_BULK, alignment, size) : xiaotai_mem_alloc(XIAOTAI_MEM_BULK, size);
}

/* Session task. Whether what is missing fits beside what has to stay free: a
 * session that cannot have everything takes nothing. */
static bool memory_allows(const video_session_config_t *config, unsigned first, unsigned limit, size_t frame_bytes)
{
    size_t total = 0, largest = 0;
    for (unsigned i = 0; i < rx.slot_count; ++i) {
        if (rx.slots[i].bytes) continue;
        total += rx.slot_bytes;
        largest = rx.slot_bytes;
    }
    for (unsigned i = first; i < limit; ++i) {
        if (rx.frames[i].bytes && rx.frames[i].capacity >= frame_bytes) continue;
        total += frame_bytes;
        if (frame_bytes > largest) largest = frame_bytes;
    }
    total += video_present_missing(config, &largest);
    xiaotai_mem_stats_t memory;
    xiaotai_mem_stats(XIAOTAI_MEM_BULK, &memory);
    return !total || !memory.total || (memory.free >= total + VIDEO_ON_DEMAND_RESERVE && memory.largest_block >= largest);
}

/* Session task. The memory this session needs: received frames, the decoder's
 * output and the finished pictures. A frame of another session that is still
 * lent out keeps its memory; the session tries again. */
static xiaotai_err_t take_pieces(const video_session_config_t *config, const xiaotai_video_decoder_t *backend)
{
    size_t size[2] = {0}, alignment[2] = {1, 1};
    if (!reserve_decode(&config->downlink, backend, size, alignment)) return XIAOTAI_ERR_INVALID_SIZE;
    const unsigned load = backend->load == XIAOTAI_VIDEO_LOAD_HARDWARE;
    if (!memory_allows(config, load ? SW_FRAMES : 0U, load ? SW_FRAMES + HW_FRAMES : rx.sw_frames, size[load]))
        return XIAOTAI_ERR_NO_MEM;
    for (unsigned i = 0; i < rx.slot_count; ++i) {
        if (rx.slots[i].bytes) continue;
        uint8_t *bytes = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, rx.slot_bytes);
        if (!bytes) return XIAOTAI_ERR_NO_MEM;
        rx.slots[i].bytes = bytes;
    }
    const unsigned first = load ? SW_FRAMES : 0U, limit = load ? SW_FRAMES + HW_FRAMES : rx.sw_frames;
    for (unsigned i = first; i < limit; ++i) {
        decoded_t *frame = &rx.frames[i];
        uint8_t *old = NULL;
        lock();
        const bool fits = frame->bytes && frame->capacity >= size[load] && !((uintptr_t)frame->bytes % alignment[load]);
        const bool lent = !fits && frame->state != FRAME_FREE;
        if (!fits && !lent) {
            old = frame->bytes;
            frame->bytes = NULL;
            frame->capacity = 0;
        }
        unlock();
        if (fits) continue;
        if (lent) return XIAOTAI_ERR_BUSY;
        xiaotai_mem_free(old);
        uint8_t *bytes = frame_memory(size[load], alignment[load]);
        if (!bytes) return XIAOTAI_ERR_NO_MEM;
        lock();
        frame->bytes = bytes;
        frame->capacity = size[load];
        unlock();
    }
    return video_present_take(config);
}

/* All of it or nothing: what a session that does not begin took goes back. */
static xiaotai_err_t take_memory(const video_session_config_t *config, const xiaotai_video_decoder_t *backend)
{
    const xiaotai_err_t err = take_pieces(config, backend);
    if (err != XIAOTAI_OK) (void)video_downlink_release_idle();
    return err;
}

bool video_downlink_release_idle(void)
{
    if (!video_engine.downlink_on_demand || !rx.slots) return true;
    uint8_t *frames[SW_FRAMES + HW_FRAMES] = {0};
    lock();
    bool idle = !rx.revision && !rx.writers && !rx.busy && xiaotai_queue_count(&rx.free) == rx.slot_count;
    for (size_t i = 0; i < SW_FRAMES + HW_FRAMES; ++i) idle &= rx.frames[i].state == FRAME_FREE;
    for (size_t i = 0; idle && i < SW_FRAMES + HW_FRAMES; ++i) {
        frames[i] = rx.frames[i].bytes;
        rx.frames[i].bytes = NULL;
        rx.frames[i].capacity = 0;
    }
    unlock();
    if (!idle) return false;
    /* No revision is in effect, so nobody takes a slot from the free queue;
     * only this task puts one into effect. */
    for (unsigned i = 0; i < rx.slot_count; ++i) {
        xiaotai_mem_free(rx.slots[i].bytes);
        rx.slots[i].bytes = NULL;
    }
    for (size_t i = 0; i < SW_FRAMES + HW_FRAMES; ++i) xiaotai_mem_free(frames[i]);
    return video_present_release_idle();
}

static xiaotai_err_t begin(const video_session_config_t *config, const xiaotai_video_transform_config_t *plan)
{
    (void)plan;  /* W8 negotiates formats against the actual decoded frame. */
    const xiaotai_video_decoder_t *backend = decoder(&config->downlink);
    if (!backend || (config->downlink.max_frame_bytes > rx.slot_bytes &&
                     rx.slot_bytes != video_engine.downlink_packet_bytes)) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (video_engine.downlink_on_demand) {
        const xiaotai_err_t memory = take_memory(config, backend);
        if (memory != XIAOTAI_OK) return memory;
    }
    lock();
    if (rx.busy) { unlock(); return XIAOTAI_ERR_BUSY; }
    const xiaotai_err_t present = video_present_begin(config);
    if (present != XIAOTAI_OK) { unlock(); return present; }
    rx.revision = config->revision;
    rx.generation = config->generation;
    ++rx.epoch;
    rx.config = config->downlink;
    rx.backend = backend;
    rx.require_recovery = rx.quarantined = false;
    unlock();
    return XIAOTAI_OK;
}

static void stop(uint32_t revision)
{
    lock();
    if (revision && rx.revision == revision) {
        video_present_stop(revision);
        rx.revision = 0;
        rx.generation = 0;
        ++rx.epoch;
        drain(&rx.incoming);
        drain(&rx.pending);
        discard_ready();
    }
    unlock();
}

static bool receive(uint32_t revision, const xiaotai_video_packet_t *packet)
{
    lock();
    const bool h264 = rx.config.codec == XIAOTAI_VIDEO_CODEC_H264;
    if (revision != rx.revision || rx.quarantined || !packet->size || packet->size > rx.slot_bytes) {
        /* A frame the limit keeps out breaks the chain like a lost one. */
        if (h264 && revision == rx.revision && packet->size > rx.slot_bytes &&
            packet->size <= rx.config.max_frame_bytes) rx.require_recovery = true;
        unlock(); return false;
    }
    const uint32_t epoch = rx.epoch;
    const bool hardware = rx.backend->load == XIAOTAI_VIDEO_LOAD_HARDWARE;
    uint8_t index;
    bool have = xiaotai_queue_receive(&rx.free, &index, 0);
    const bool replaced = !have && !h264 && xiaotai_queue_receive(&rx.incoming, &index, 0);
    have |= replaced;
    if (!have) {
        if (h264) rx.require_recovery = true;
        unlock();
        return false;
    }
    ++rx.writers;
    unlock();

    compressed_t *slot = &rx.slots[index];
    slot->revision = revision;
    slot->epoch = epoch;
    slot->queued_us = xiaotai_time_us();
    slot->packet = *packet;
    memcpy(slot->bytes, packet->data, packet->size);
    slot->packet.data = slot->bytes;

    lock();
    const bool accepted = revision == rx.revision && epoch == rx.epoch && !rx.quarantined &&
                          xiaotai_queue_send(&rx.incoming, &index, 0);
    if (accepted) video_present_arrival(revision, slot->queued_us);
    else returned(index);
    unlock();
    if (accepted) {
        xiaotai_signal_give(hardware ? &rx.hardware_work : &rx.relay_work);
        publish(revision, replaced ? 1U : 0U, 0, 0, 0, 0, NULL);
    }
    lock();
    --rx.writers;
    unlock();
    return accepted;
}

static const video_session_receiver_t receiver = {begin, stop, receive};

static bool reserve_decode(const xiaotai_video_downlink_t *config, const xiaotai_video_decoder_t *backend,
                           size_t size[2], size_t alignment[2])
{
    const unsigned load = backend->load == XIAOTAI_VIDEO_LOAD_HARDWARE;
    size_t width = config->max_width, height = config->max_height;
    if (config->codec != XIAOTAI_VIDEO_CODEC_JPEG) {
        width = (width + 15U) & ~(size_t)15U;
        height = (height + 15U) & ~(size_t)15U;
    }
    /* The largest supported pixel (RGB888) needs three bytes. Check before
     * using the inline image-size helper on a 32-bit target. */
    if (width > UINT16_MAX || height > UINT16_MAX || width > SIZE_MAX / height / 3U) return false;
    size_t n = xiaotai_video_image_bytes(backend->output, (uint16_t)width, (uint16_t)height);
    size_t a = backend->output_alignment;
    if (!n || (a && (a & (a - 1U)))) return false;
    if (n > size[load]) size[load] = n;
    if (a > alignment[load]) alignment[load] = a;
    return true;
}

bool video_downlink_reset(void)
{
    if (!rx.slots) return true;
    /* Workers have left; a push that passed the public started check may
     * still hold a slot. Cancel admission before waiting for that one copy. */
    stop(rx.revision);
    for (unsigned ms = 0;; ++ms) {
        lock();
        bool pinned = rx.writers != 0;
        for (size_t i = 0; i < SW_FRAMES + HW_FRAMES; ++i) pinned |= rx.frames[i].state == FRAME_LEASED;
        unlock();
        if (!pinned) break;
        if (ms == VIDEO_TASK_EXIT_WAIT_MS) return false;
        xiaotai_delay_ms(1);
    }
    if (rx.queue_count > 2) xiaotai_queue_deinit(&rx.pending);
    if (rx.queue_count > 1) xiaotai_queue_deinit(&rx.incoming);
    if (rx.queue_count > 0) xiaotai_queue_deinit(&rx.free);
    if (rx.signal_count > 1) xiaotai_signal_deinit(&rx.hardware_work);
    if (rx.signal_count > 0) xiaotai_signal_deinit(&rx.relay_work);
    for (unsigned i = 0; i < rx.slot_count; ++i) xiaotai_mem_free(rx.slots[i].bytes);
    for (size_t i = 0; i < SW_FRAMES + HW_FRAMES; ++i) xiaotai_mem_free(rx.frames[i].bytes);
    xiaotai_mem_free(rx.slots);
    /* Keep the lock initialized: a late callback is allowed to see revision 0. */
    rx.slots = NULL;
    rx.slot_count = rx.queue_count = rx.signal_count = 0;
    rx.slot_bytes = 0;
    memset(rx.frames, 0, sizeof(rx.frames));
    return true;
}

xiaotai_err_t video_downlink_prepare(void)
{
    if (video_session_has_receiver()) return XIAOTAI_OK;
    if (rx.slots) return XIAOTAI_ERR_BUSY;
    if (!rx.lock_ready) { xiaotai_critical_init(&rx.lock); rx.lock_ready = true; }
    size_t slot_bytes = 0, size[2] = {0}, alignment[2] = {1, 1};
    unsigned count = 0;
    for (size_t i = 0; i < VIDEO_DOWNLINK_CODECS; ++i) {
        const xiaotai_video_downlink_t *d = &video_engine.requirements.downlinks[i];
        if (!d->codec) continue;
        const xiaotai_video_decoder_t *backend = decoder(d);
        if (!backend) continue;
        const uint64_t n = (uint64_t)d->fps * d->burst_ms / 1000U;
        if (n > UINT8_MAX || !reserve_decode(d, backend, size, alignment)) return XIAOTAI_ERR_INVALID_SIZE;
        if (count < n) count = (unsigned)n;
        if (count < VIDEO_DOWNLINK_MIN_PACKETS) count = VIDEO_DOWNLINK_MIN_PACKETS;
        if (slot_bytes < d->max_frame_bytes) slot_bytes = d->max_frame_bytes;
    }
    if (!count) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (video_engine.downlink_packets && count > video_engine.downlink_packets) count = video_engine.downlink_packets;
    if (video_engine.downlink_packet_bytes && slot_bytes > video_engine.downlink_packet_bytes)
        slot_bytes = video_engine.downlink_packet_bytes;
    rx.sw_frames = video_engine.downlink_pictures && video_engine.downlink_pictures < SW_FRAMES ?
                   video_engine.downlink_pictures : SW_FRAMES;
    rx.slots = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, count, sizeof(*rx.slots));
    if (!rx.slots) return XIAOTAI_ERR_NO_MEM;
    rx.slot_count = count;
    rx.slot_bytes = slot_bytes;
    xiaotai_queue_t *queues[] = {&rx.free, &rx.incoming, &rx.pending};
    for (size_t i = 0; i < sizeof(queues) / sizeof(*queues); ++i) {
        if (xiaotai_queue_init(queues[i], count, sizeof(uint8_t), XIAOTAI_MEM_BULK) != XIAOTAI_OK) goto failed;
        ++rx.queue_count;
    }
    if (xiaotai_signal_init(&rx.relay_work) != XIAOTAI_OK) goto failed;
    ++rx.signal_count;
    if (xiaotai_signal_init(&rx.hardware_work) != XIAOTAI_OK) goto failed;
    ++rx.signal_count;
    const bool now = !video_engine.downlink_on_demand;
    for (unsigned i = 0; i < count; ++i) {
        if (now) rx.slots[i].bytes = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, slot_bytes);
        if (now && !rx.slots[i].bytes) goto failed;
        returned((uint8_t)i);
    }
    for (unsigned i = 0; now && i < SW_FRAMES + HW_FRAMES; ++i) {
        const unsigned load = i >= SW_FRAMES;
        if (!size[load] || (!load && i >= rx.sw_frames)) continue;
        rx.frames[i].bytes = frame_memory(size[load], alignment[load]);
        if (!rx.frames[i].bytes) goto failed;
        rx.frames[i].capacity = size[load];
    }
    video_session_install_receiver(&receiver);
    return XIAOTAI_OK;
failed:
    (void)video_downlink_reset();
    return XIAOTAI_ERR_NO_MEM;
}

static void close_decoder(worker_t *worker)
{
    if (worker->state && !(worker->backend->flags & XIAOTAI_VIDEO_DECODER_PERSISTENT))
        worker->backend->close(worker->state);
    worker->state = NULL;
}

static void break_reference(worker_t *worker, uint32_t revision, bool clear_queue)
{
    close_decoder(worker);
    video_h264_chain_reset(&worker->chain);
    worker->drop_to_key = false;
    worker->late_samples = 0;
    lock();
    if (rx.revision == revision) {
        if (clear_queue) {
            worker->epoch = ++rx.epoch;
            drain(&rx.incoming);
            drain(&rx.pending);
        }
        discard_ready();
    }
    unlock();
    video_session_downlink_flags(revision, true, false);
    video_session_downlink_presentable(revision, false);
}

static int acquire_frame(bool hardware)
{
    int index = -1;
    const int limit = hardware ? SW_FRAMES + HW_FRAMES : SW_FRAMES;
    lock();
    for (int i = hardware ? SW_FRAMES : 0; i < limit; ++i) {
        const decoded_t *slot = &rx.frames[i];
        if (!slot->bytes) continue;
        if (slot->state == FRAME_FREE) { index = i; break; }
        if (slot->state == FRAME_READY && (index < 0 || slot->order < rx.frames[index].order)) index = i;
    }
    if (index >= 0) rx.frames[index].state = FRAME_WRITING;
    unlock();
    return index;
}

static bool valid_image(const decoded_t *slot, const xiaotai_video_image_t *image, const xiaotai_video_downlink_t *d)
{
    const unsigned w = image->width, h = image->height, rows = image->rows;
    const unsigned stride = image->stride ? image->stride : w;
    if (image->data != slot->bytes || image->size > slot->capacity || !w || !h ||
        h > rows || w > stride || stride > SIZE_MAX / rows / 3U) return false;
    if (image->pixel == XIAOTAI_VIDEO_PIXEL_I420 && ((w | h) & 1U)) return false;
    const size_t size = xiaotai_video_image_bytes(image->pixel, (uint16_t)stride, (uint16_t)rows);
    if (!size || size > image->size) return false;
    if (d->codec == XIAOTAI_VIDEO_CODEC_H264)
        return w >= 16U && h >= 16U && !(w & 1U) && !(h & 1U) && w == d->max_width && h == d->max_height;
    const unsigned max = d->max_width > d->max_height ? d->max_width : d->max_height;
    return w <= max && h <= max && (uint64_t)((w + 15U) & ~15U) * ((h + 15U) & ~15U) <=
                                  (uint64_t)d->max_width * d->max_height;
}

static void pack_i420(decoded_t *slot, xiaotai_video_image_t *image)
{
    if (image->pixel != XIAOTAI_VIDEO_PIXEL_I420) return;
    const size_t width = image->width, height = image->height, rows = image->rows;
    const size_t stride = image->stride ? image->stride : width;
    if (width == stride && height == rows) return;
    const size_t old_y = stride * rows, new_y = width * height;
    /* The transform ABI places each plane immediately after the visible
     * previous one. Compact the decoder's own slot before lending it. */
    for (size_t y = 0; y < height; ++y)
        memmove(slot->bytes + y * width, slot->bytes + y * stride, width);
    for (size_t plane = 0; plane < 2U; ++plane) {
        uint8_t *to = slot->bytes + new_y + plane * new_y / 4U;
        const uint8_t *from = slot->bytes + old_y + plane * old_y / 4U;
        for (size_t y = 0; y < height / 2U; ++y)
            memmove(to + y * width / 2U, from + y * stride / 2U, width / 2U);
    }
    image->stride = image->width;
    image->rows = image->height;
    image->size = new_y + new_y / 2U;
}

void video_downlink_monitor(int64_t now_us)
{
    if (!rx.slots) return;
    lock();
    const bool expired = rx.busy && !rx.quarantined && now_us >= rx.watched_since &&
                         now_us - rx.watched_since >= STALL_MS * INT64_C(1000);
    const uint32_t revision = rx.watched_revision;
    const uint32_t generation = rx.watched_generation;
    if (expired) {
        rx.quarantined = true;
        discard_ready();
        video_session_downlink_quarantine(revision, true);
        video_session_downlink_presentable(revision, false);
    }
    unlock();
    if (!expired) return;
    xiaotai_eventlog_error(VIDEO_EVENTLOG_SOURCE, "decode.stalled", generation, XIAOTAI_ERR_TIMEOUT);
}

static bool process(worker_t *worker, uint8_t index, bool hardware)
{
    bool ran_decoder = false, frame_ready = false;
    compressed_t *input = &rx.slots[index];
    const uint32_t revision = input->revision;
    lock();
    const bool current = revision == rx.revision && input->epoch == rx.epoch && !rx.quarantined &&
                         (rx.backend->load == XIAOTAI_VIDEO_LOAD_HARDWARE) == hardware;
    const xiaotai_video_downlink_t cfg = rx.config;
    const uint32_t generation = rx.generation;
    const xiaotai_video_decoder_t *backend = rx.backend;
    const bool overflow = current && rx.require_recovery;
    if (overflow) rx.require_recovery = false;
    unlock();
    if (!current) goto done;
    const bool h264 = cfg.codec == XIAOTAI_VIDEO_CODEC_H264;
    if (worker->revision != revision || worker->epoch != input->epoch) {
        close_decoder(worker);
        *worker = (worker_t){.revision = revision, .epoch = input->epoch, .backend = backend};
        video_h264_chain_reset(&worker->chain);
    }
    if (h264) {
        const unsigned depth = queued();
        const int64_t now = xiaotai_time_us();
        const int64_t age = now >= input->queued_us ? now - input->queued_us : 0;
        const bool buffering = video_session_downlink_buffering(revision);
        const int64_t late = LATE_FRAME_INTERVALS * INT64_C(1000000) / (cfg.fps ? cfg.fps : 1);
        if (!buffering && age >= late && depth >= LATE_QUEUE) {
            if (worker->late_samples < LATE_SAMPLES) ++worker->late_samples;
        } else if (buffering || age < late / 2 || depth < LATE_QUEUE / 2) {
            worker->late_samples = 0;
        }
        if ((overflow || worker->late_samples >= LATE_SAMPLES) && !worker->drop_to_key && !worker->chain.unsupported) {
            break_reference(worker, revision, false);
            worker->drop_to_key = true;
            publish(revision, 0, 0, 0, 0, 1, NULL);
        }
        const video_h264_au_t au = video_h264_inspect(input->packet.data, input->packet.size);
        const video_h264_admission_t admission = video_h264_admit(&worker->chain, &au, input->packet.key_frame,
            (backend->flags & XIAOTAI_VIDEO_DECODER_BASELINE_ONLY) != 0, cfg.recover_h264_profile,
            cfg.preserve_h264_parameter_chain);
        if (admission.reference_changed || admission.status == VIDEO_H264_UNSUPPORTED) {
            close_decoder(worker);
            lock();
            if (rx.revision == revision) discard_ready();
            unlock();
            if (admission.reference_changed) {
                video_session_downlink_presentable(revision, false);
                publish(revision, 0, 0, 0, 0, 1, NULL);
            }
        }
        video_session_downlink_flags(revision, worker->chain.waiting_key, worker->chain.unsupported);
        if (admission.status != VIDEO_H264_ADMIT) {
            if (admission.status != VIDEO_H264_UNSUPPORTED) video_session_downlink_need_key(revision);
            publish(revision, admission.status == VIDEO_H264_WAIT_KEY ? 1U : 0U,
                    admission.status != VIDEO_H264_WAIT_KEY ? 1U : 0U, 0, 0, 0, NULL);
            goto done;
        }
        if (admission.key) worker->drop_to_key = false;
        input->packet.key_frame = admission.key;
    }

    const int n = acquire_frame(hardware);
    if (n < 0) {
        if (h264) break_reference(worker, revision, true);
        publish(revision, 1, 0, 0, 0, h264, NULL);
        goto done;
    }
    lock();
    const bool decode = rx.revision == revision && rx.epoch == input->epoch && !rx.busy;
    if (decode) {
        rx.busy = true;
        rx.watched_revision = revision;
        rx.watched_generation = generation;
        rx.watched_since = xiaotai_time_us();
    }
    unlock();
    decoded_t *slot = &rx.frames[n];
    xiaotai_video_image_t image = {0};
    xiaotai_err_t err = XIAOTAI_ERR_INVALID_STATE;
    if (decode) {
        const xiaotai_video_decoder_config_t limits = {
            .codec = cfg.codec, .max_width = cfg.max_width, .max_height = cfg.max_height,
        };
        err = XIAOTAI_OK;
        if (!worker->state) {
            if (backend->flags & XIAOTAI_VIDEO_DECODER_PERSISTENT)
                worker->state = video_registry_persistent_state(backend);
            else err = backend->open(&limits, &worker->state);
            if (err == XIAOTAI_OK && !worker->state) err = XIAOTAI_FAIL;
        }
        if (err == XIAOTAI_OK) {
            ran_decoder = true;
            if (backend->decode_configured)
                err = backend->decode_configured(worker->state, &limits, &input->packet, slot->bytes, slot->capacity, &image);
            else
                err = backend->decode(worker->state, &input->packet, slot->bytes, slot->capacity, &image);
        }
        if (err == XIAOTAI_OK && (image.pixel != backend->output || !valid_image(slot, &image, &cfg)))
            err = XIAOTAI_ERR_INVALID_SIZE;
        if (err == XIAOTAI_OK) pack_i420(slot, &image);
        if (err == XIAOTAI_OK && h264 && worker->chain.waiting_key) err = XIAOTAI_ERR_NOT_FOUND;
    }
    lock();
    const bool stalled = decode && rx.quarantined;
    const bool valid = decode && rx.revision == revision && rx.epoch == input->epoch;
    if (decode) {
        rx.busy = rx.quarantined = false;
        rx.watched_revision = rx.watched_generation = 0;
        if (stalled) video_session_downlink_quarantine(revision, false);
    }
    if (valid && !stalled && err == XIAOTAI_OK) {
        slot->order = ++rx.ready_order;
        slot->frame = (video_downlink_frame_t){.image = image, .revision = revision, .generation = generation, .slot = (uint8_t)n,
            .timestamp_ms = input->packet.timestamp_ms, .key_frame = input->packet.key_frame,
            .reference = rx.reference,
            .lease_cookie = slot->order};
        slot->state = FRAME_READY;
        frame_ready = true;
    } else slot->state = FRAME_FREE;
    unlock();
    if (!valid) goto done;
    if (stalled) err = XIAOTAI_ERR_TIMEOUT;
    worker->starved = err != XIAOTAI_ERR_NO_MEM ? 0U : worker->starved < UINT8_MAX ? worker->starved + 1U : UINT8_MAX;
    if (video_engine.downlink_on_demand && worker->starved >= VIDEO_DECODER_STARVED) {
        /* Each key frame would only fail again, with everything else held. */
        worker->starved = 0;
        close_decoder(worker);
        video_h264_chain_reset(&worker->chain);
        publish(revision, 0, 0, 0, 1, 0, NULL);
        video_session_downlink_starved(revision);
        goto done;
    }
    if (err == XIAOTAI_ERR_NOT_SUPPORTED && h264) {
        break_reference(worker, revision, true);
        worker->chain.unsupported = true;
        video_session_downlink_flags(revision, false, true);
    } else if (h264 && err != XIAOTAI_OK && err != XIAOTAI_ERR_NOT_FOUND) {
        break_reference(worker, revision, true);
        publish(revision, 0, 0, 0, 1, 1, NULL);
    } else if (err == XIAOTAI_OK) {
        publish(revision, 0, 0, 1, 0, 0, &image);
    } else if (err != XIAOTAI_ERR_NOT_FOUND) {
        publish(revision, 0, err == XIAOTAI_ERR_INVALID_SIZE, 0, err != XIAOTAI_ERR_INVALID_SIZE, 0, NULL);
    }
done:
    returned(index);
    publish(revision, 0, 0, 0, 0, 0, NULL);
    if (frame_ready) video_convert_kick(hardware);
    return ran_decoder;
}

static bool take_decoded(video_downlink_frame_t *out, bool convert_only, bool hardware)
{
    if (!out || !rx.slots) return false;
    lock();
    if (!rx.revision || rx.quarantined || !video_session_downlink_current(rx.revision)) { unlock(); return false; }
    const size_t first = convert_only && hardware ? SW_FRAMES : 0U;
    const size_t limit = convert_only && !hardware ? SW_FRAMES : SW_FRAMES + HW_FRAMES;
    const bool newest = convert_only && (hardware || !video_session_downlink_buffering(rx.revision));
    int selected = -1;
    for (size_t i = first; i < limit; ++i)
        if (rx.frames[i].state == FRAME_READY && rx.frames[i].frame.revision == rx.revision &&
            (selected < 0 || (newest ? rx.frames[i].order > rx.frames[selected].order :
                                       rx.frames[i].order < rx.frames[selected].order))) selected = (int)i;
    if (selected >= 0) {
        rx.frames[selected].state = FRAME_LEASED;
        *out = rx.frames[selected].frame;
        if (newest) for (size_t i = first; i < limit; ++i)
            if (rx.frames[i].state == FRAME_READY && rx.frames[i].frame.revision == rx.revision)
                rx.frames[i].state = FRAME_FREE;
    }
    unlock();
    return selected >= 0;
}

bool video_downlink_take_decoded(video_downlink_frame_t *out) { return take_decoded(out, false, false); }

bool video_downlink_take_for_conversion(bool hardware, video_downlink_frame_t *out)
{
    return take_decoded(out, true, hardware);
}

void video_downlink_finish_conversion(const video_downlink_frame_t *decoded, video_present_write_t *write,
                                      const xiaotai_video_frame_t *frame)
{
    /* rx -> present / session: a flush cannot split final-frame publication
     * and the remote-availability edge. No backend is called under this lock. */
    lock();
    if (rx.revision == decoded->revision && rx.reference == decoded->reference && !rx.quarantined &&
        video_session_downlink_current(decoded->revision)) {
        if (video_present_publish(write, frame)) {
            video_session_downlink_presentable(decoded->revision, true);
            video_session_downlink_converted(decoded->revision, true);
        }
    } else video_present_cancel(write);
    unlock();
}

void video_downlink_release_decoded(video_downlink_frame_t *frame)
{
    if (!frame || !frame->revision || frame->slot >= SW_FRAMES + HW_FRAMES) return;
    lock();
    decoded_t *slot = &rx.frames[frame->slot];
    if (slot->state == FRAME_LEASED && slot->order == frame->lease_cookie && slot->frame.revision == frame->revision &&
        slot->bytes == frame->image.data)
        slot->state = FRAME_FREE;
    unlock();
    memset(frame, 0, sizeof(*frame));
}

void video_downlink_relay_task(void *arg)
{
    video_task_t *task = arg;
    while (!atomic_load(&task->stop)) {
        uint8_t index;
        if (!video_started() || !rx.slots) { xiaotai_delay_ms(WAIT_MS); continue; }
        /* The hardware worker consumes ingress directly. The relay cannot
         * wait on that queue: its next frame might belong to hardware. */
        lock();
        const bool software = rx.revision && rx.backend->load == XIAOTAI_VIDEO_LOAD_SOFTWARE;
        bool moved = false;
        if (software && xiaotai_queue_receive(&rx.incoming, &index, 0)) {
            moved = xiaotai_queue_send(&rx.pending, &index, 0);
            if (!moved) returned(index);
        }
        unlock();
        if (!moved) (void)xiaotai_signal_take(&rx.relay_work, WAIT_MS);
    }
    xiaotai_signal_give(&task->exited);
    xiaotai_task_exit();
}

static void decode_task(void *arg, bool hardware)
{
    video_task_t *task = arg;
    worker_t worker = {0};
    while (!atomic_load(&task->stop)) {
        uint8_t index;
        if (!video_started() || !rx.slots) { xiaotai_delay_ms(WAIT_MS); continue; }
        bool have = !hardware && xiaotai_queue_receive(&rx.pending, &index, WAIT_MS);
        lock();
        const uint32_t revision = rx.revision;
        const bool selected = revision && (rx.backend->load == XIAOTAI_VIDEO_LOAD_HARDWARE) == hardware;
        unsigned superseded = 0;
        if (selected && hardware) {
            xiaotai_queue_t *queue = &rx.incoming;
            have = xiaotai_queue_receive(queue, &index, 0);
            uint8_t newer;
            while (hardware && have && xiaotai_queue_receive(queue, &newer, 0)) {
                returned(index);
                index = newer;
                ++superseded;
            }
        }
        unlock();
        if (worker.revision && worker.revision != revision) close_decoder(&worker);
        if (!have) {
            if (hardware) (void)xiaotai_signal_take(&rx.hardware_work, WAIT_MS);
            continue;
        }
        if (superseded) publish(revision, superseded, 0, 0, 0, 0, NULL);
        const bool ran_decoder = process(&worker, index, hardware);
        if (!hardware && ran_decoder) {
            if (++worker.decoded_in_cycle == 30U) { worker.decoded_in_cycle = 0; xiaotai_delay_ms(5); }
            else if (!xiaotai_queue_count(&rx.pending)) xiaotai_delay_ms(1);
        }
    }
    close_decoder(&worker);
    xiaotai_signal_give(&task->exited);
    xiaotai_task_exit();
}

void video_downlink_decode_sw_task(void *arg) { decode_task(arg, false); }
void video_downlink_decode_hw_task(void *arg) { decode_task(arg, true); }
