// SPDX-License-Identifier: MIT
// Replaces legacy: tests/native/wt99/test_camera_frame_layout.c
// Replaces legacy: tests/regression/test_p4_camera_frame_contract.py
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "camera_csi_fake.h"
#include "xiaotai_mem_host.h"

static bool external, external_pointer = true;
static xiaotai_i2c_bus_t *bus;
/* Created parts, their mutexes and resident frame slabs are lifetime objects. */
static const xiaotai_camera_t *parts[20];
static unsigned part_count, power_on, power_off;
static xiaotai_err_t power_failure;
static size_t expected_alignment;
static unsigned aligned_allocations;

bool __wrap_xiaotai_mem_has_external(void) { return external; }
bool __wrap_xiaotai_mem_is_external(const void *ptr) { return ptr && external && external_pointer; }

void *__real_xiaotai_mem_aligned(xiaotai_mem_kind_t kind, size_t alignment, size_t size);
void *__wrap_xiaotai_mem_aligned(xiaotai_mem_kind_t kind, size_t alignment, size_t size)
{
    if (expected_alignment && kind == XIAOTAI_MEM_BULK) {
        assert(alignment == expected_alignment);
        ++aligned_allocations;
    }
    return __real_xiaotai_mem_aligned(kind, alignment, size);
}

static xiaotai_err_t supply(void *self, bool on)
{
    assert(self == &csi_fake);
    if (power_failure) return power_failure;
    if (on) ++power_on;
    else ++power_off;
    csi_fake.powered = on;
    return XIAOTAI_OK;
}

static xiaotai_camera_csi_config_t base_config(void)
{
    csi_fake = (csi_fake_t){0};
    external = external_pointer = true;
    power_on = power_off = 0;
    power_failure = XIAOTAI_OK;
    expected_alignment = 0;
    aligned_allocations = 0;
    xiaotai_mem_host_clear();
    return (xiaotai_camera_csi_config_t){.sccb = bus, .sensor_address = 0x36, .width = 64, .height = 32,
        .fps = 30, .row_align = 16, .power = {.set = supply, .self = &csi_fake}};
}

static const xiaotai_camera_t *create(const xiaotai_camera_csi_config_t *cfg)
{
    assert(part_count < sizeof(parts) / sizeof(parts[0]));
    assert(xiaotai_camera_csi_create(cfg, &parts[part_count]) == XIAOTAI_OK);
    assert(parts[part_count] && power_off == 1 && !power_on && csi_fake.call_count == 0);
    return parts[part_count++];
}

static xiaotai_camera_mode_t mode(const xiaotai_camera_t *p)
{
    return (xiaotai_camera_mode_t){.width = p->desc.max_width, .height = p->desc.max_height,
                                  .format = XIAOTAI_CAMERA_YUV420_PAIRED, .fps = 27};
}

static xiaotai_camera_csi_stats_t stats(const xiaotai_camera_t *p)
{
    xiaotai_camera_csi_stats_t out;
    xiaotai_camera_csi_stats(p, &out);
    return out;
}

static void fail(csi_fake_call_t op, xiaotai_err_t err)
{
    csi_fake.fail_call = op;
    csi_fake.failure = err;
}

static void push(camera_csi_buffer_t a, const camera_csi_buffer_t *b)
{
    csi_fake.frame_next = 0;
    csi_fake.frame_count = b ? 2 : 1;
    csi_fake.frames[0] = a;
    if (b) csi_fake.frames[1] = *b;
}

static void test_validation_and_pool(void)
{
    xiaotai_camera_csi_config_t cfg = base_config();
    const xiaotai_camera_t *p = NULL;
    external = false;
    assert(xiaotai_camera_csi_create(&cfg, &p) == XIAOTAI_ERR_NOT_SUPPORTED && !p);
    assert(power_on == 0 && power_off == 0 && xiaotai_mem_host_allocations() == 0);
    external = true;
    cfg.sccb_hz = 400001;
    assert(xiaotai_camera_csi_create(&cfg, &p) == XIAOTAI_ERR_INVALID_ARG && !p);
    cfg.sccb_hz = 0;
    cfg.width--;
    assert(xiaotai_camera_csi_create(&cfg, &p) == XIAOTAI_ERR_INVALID_ARG && !p);
    cfg.width++;
    cfg.height--;
    assert(xiaotai_camera_csi_create(&cfg, &p) == XIAOTAI_ERR_INVALID_ARG && !p);
    cfg.height = 32;
    cfg.row_align = 7; /* Paired YUV cannot expose an odd physical row count. */
    assert(xiaotai_camera_csi_create(&cfg, &p) == XIAOTAI_ERR_INVALID_SIZE && !p);
    cfg.row_align = 16;
    power_failure = XIAOTAI_ERR_TIMEOUT;
    assert(xiaotai_camera_csi_create(&cfg, &p) == XIAOTAI_ERR_TIMEOUT && !p);
    assert(xiaotai_mem_host_outstanding() == 0);
    power_failure = XIAOTAI_OK;
    p = create(&cfg);
    assert(p->desc.max_fps == 30 && p->desc.formats == XIAOTAI_CAMERA_YUV420_PAIRED);
    assert(p->desc.min_width == 64 && p->desc.min_height == 32 && p->desc.max_width == 64 && p->desc.max_height == 32);
    assert(stats(p).pool_bytes == 0);

    for (unsigned n = 1; n <= 3; ++n) {
        xiaotai_mem_host_clear();
        xiaotai_mem_host_fail_nth(n);
        assert(p->prepare(p->self) == XIAOTAI_ERR_NO_MEM);
        assert(stats(p).pool_bytes == 0 && power_on == 0 && csi_fake.call_count == 0);
        assert(xiaotai_mem_host_outstanding() == 0);
    }
    external_pointer = false;
    assert(p->prepare(p->self) == XIAOTAI_ERR_NO_MEM && !stats(p).pool_bytes);
    assert(xiaotai_mem_host_outstanding() == 0);
    external_pointer = true;
    fail(CSI_GUARD, XIAOTAI_ERR_NO_MEM);
    assert(p->prepare(p->self) == XIAOTAI_ERR_NO_MEM && !csi_fake.powered);
    assert(csi_fake.guard_calls == 1 && csi_fake.start_calls == 0 && !csi_fake.guard);
    const size_t total = 3U * 64U * 32U * 3U / 2U;
    assert(stats(p).pool_bytes == total);
    unsigned allocations = xiaotai_mem_host_allocations();
    fail(CSI_START, XIAOTAI_FAIL);
    assert(p->set_power(p->self, true) == XIAOTAI_FAIL && !csi_fake.powered);
    assert(csi_fake.guard_calls == 2 && csi_fake.start_calls == 1 && csi_fake.guard);
    fail(CSI_START, XIAOTAI_OK);
    assert(p->set_power(p->self, true) == XIAOTAI_OK && csi_fake.powered);
    assert(csi_fake.guard_calls == 2 && csi_fake.start_calls == 2 && csi_fake.hz == 100000);
    assert(p->set_power(p->self, false) == XIAOTAI_OK && !csi_fake.powered);
    assert(p->set_power(p->self, true) == XIAOTAI_OK && csi_fake.powered);
    assert(csi_fake.start_calls == 2 && xiaotai_mem_host_allocations() == allocations);
}

static void check_unpadded_capture_contract(const xiaotai_camera_t *p, size_t size)
{
    xiaotai_camera_frame_t first = {0}, second = {0};
    const unsigned returns = csi_fake.returns;
    assert(p->borrow(p->self, &first) == XIAOTAI_ERR_NOT_FOUND && csi_fake.returns == returns);
    const camera_csi_buffer_t latest = {.index = 1, .sequence = 7, .captured_us = 99};
    push((camera_csi_buffer_t){.index = 0, .bytes = size}, &latest);
    assert(p->borrow(p->self, &first) == XIAOTAI_OK && first.data == csi_fake.pool[1]);
    assert(first.width == 1280 && first.height == 960 && first.rows == 960 && first.size == size);
    assert(first.format == XIAOTAI_CAMERA_YUV420_PAIRED && first.sequence == 7 && first.captured_us == 99);
    assert(first.skipped == 1 && csi_fake.returns == returns + 1U);
    assert(csi_fake.queued[0] && !csi_fake.queued[1]);
    assert(p->borrow(p->self, &second) == XIAOTAI_ERR_INVALID_STATE && !second.data);
    assert(p->close(p->self) == XIAOTAI_ERR_INVALID_STATE && csi_fake.streaming);
    assert(p->set_power(p->self, false) == XIAOTAI_ERR_INVALID_STATE && csi_fake.powered);
    xiaotai_camera_frame_t stale = first;
    assert(p->release(p->self, &first) == XIAOTAI_OK && !first.data && csi_fake.returns == returns + 2U);
    push((camera_csi_buffer_t){.index = 2, .bytes = size}, NULL);
    assert(p->borrow(p->self, &second) == XIAOTAI_OK && second.data == csi_fake.pool[2]);
    assert(p->release(p->self, &stale) == XIAOTAI_ERR_INVALID_STATE && csi_fake.returns == returns + 2U);
    assert(p->release(p->self, &second) == XIAOTAI_OK && !second.data && csi_fake.returns == returns + 3U);

    const camera_csi_buffer_t bad[] = {
        {.index = 0, .bytes = size - 1U}, {.index = 0, .bytes = size + 1U}, {.index = 0, .error = true},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        push(bad[i], NULL);
        const xiaotai_err_t expected = bad[i].error ? XIAOTAI_FAIL : XIAOTAI_ERR_INVALID_SIZE;
        assert(p->borrow(p->self, &first) == expected && !first.data);
        assert(csi_fake.queued[0] && csi_fake.returns == returns + 4U + i);
    }
    push((camera_csi_buffer_t){.index = 1, .bytes = size}, &bad[0]);
    assert(p->borrow(p->self, &first) == XIAOTAI_ERR_INVALID_SIZE && !first.data);
    assert(csi_fake.queued[0] && csi_fake.queued[1] && csi_fake.returns == returns + 8U);
    puts("CSI unpadded: no frame, newest loan, active-close refusal and rejected frames checked");
}

static void test_waveshare(void)
{
    xiaotai_camera_csi_config_t cfg = base_config();
    static const xiaotai_sensor_reg_t anti_flicker[] = {{0x3a00, 0x20, 0x20}, {0x3c00, 0x04, 0x04}, {0x3c01, 0x80, 0x80}};
    cfg.setup = anti_flicker;
    cfg.setup_count = sizeof(anti_flicker) / sizeof(anti_flicker[0]);
    cfg.width = 1280;
    cfg.height = 960;
    cfg.rotation = 1;
    expected_alignment = csi_fake.cache_alignment = 128U;
    csi_fake.reg[0x3a00] = 0x01;
    csi_fake.reg[0x3c00] = 0x02;
    csi_fake.reg[0x3c01] = 0x03;
    const xiaotai_camera_t *p = create(&cfg);
    assert(p->desc.rotation == 1 && !p->desc.flags);
    xiaotai_camera_mode_t m = mode(p);
    assert(p->prepare(p->self) == XIAOTAI_OK && !csi_fake.powered && csi_fake.guard && csi_fake.ready);
    assert(csi_fake.call_count == 2 && csi_fake.calls[0] == CSI_GUARD && csi_fake.calls[1] == CSI_START);
    assert(stats(p).pool_bytes == 5529600 && csi_fake.hz == 100000);
    assert(aligned_allocations == 3);
    assert(p->set_power(p->self, true) == XIAOTAI_OK && csi_fake.start_calls == 1);
    --m.width;
    assert(p->open(p->self, &m) == XIAOTAI_ERR_NOT_SUPPORTED && !csi_fake.fd);
    ++m.width;
    csi_fake.reject_reg_write = true;
    assert(p->open(p->self, &m) == XIAOTAI_FAIL && csi_fake.closes == 1 && !csi_fake.requests);
    assert(csi_fake.calls[2] == CSI_OPEN && csi_fake.calls[3] == CSI_FORMAT && csi_fake.calls[4] == CSI_RATE);
    assert(p->close(p->self) == XIAOTAI_OK);
    csi_fake.reject_reg_write = false;
    fail(CSI_RATE, XIAOTAI_ERR_NOT_SUPPORTED);
    assert(p->open(p->self, &m) == XIAOTAI_OK && csi_fake.streaming && csi_fake.fps == 27);
    for (unsigned i = 0; i < 3; ++i) assert(((uintptr_t)csi_fake.pool[i] % 128U) == 0);
    assert(csi_fake.reg[0x3a00] == 0x21 && csi_fake.reg[0x3c00] == 0x06 && csi_fake.reg[0x3c01] == 0x83);
    assert(stats(p).rate_refused == 1 && stats(p).opens == 1 && stats(p).open_failures == 2);
    xiaotai_camera_frame_t frame = {0};
    push((camera_csi_buffer_t){.index = 0, .bytes = csi_fake.format_bytes, .sequence = 1, .captured_us = 90}, NULL);
    assert(p->borrow(p->self, &frame) == XIAOTAI_OK && frame.data == csi_fake.pool[0]);
    assert(frame.width == 1280 && frame.height == 960 && frame.rows == frame.height);
    assert(frame.captured_us == 90 && frame.sequence == 1 && frame.size == csi_fake.format_bytes);
    assert(p->release(p->self, &frame) == XIAOTAI_OK);
    push((camera_csi_buffer_t){.index = 1, .sequence = 2, .captured_us = 91}, NULL);
    assert(p->borrow(p->self, &frame) == XIAOTAI_OK && frame.data == csi_fake.pool[1]);
    assert(frame.captured_us == 91 && frame.sequence == 2 && frame.size == 1280U * 960U * 3U / 2U);
    assert(p->release(p->self, &frame) == XIAOTAI_OK);
    check_unpadded_capture_contract(p, csi_fake.format_bytes);
    assert(p->close(p->self) == XIAOTAI_OK && !csi_fake.fd);
    assert(csi_fake.calls[csi_fake.call_count - 3] == CSI_STREAM_OFF);
    assert(csi_fake.calls[csi_fake.call_count - 2] == CSI_FREE);
    assert(csi_fake.calls[csi_fake.call_count - 1] == CSI_CLOSE);
    assert(p->set_power(p->self, false) == XIAOTAI_OK && !csi_fake.powered);
    assert(p->set_power(p->self, true) == XIAOTAI_OK);
    assert(p->open(p->self, &m) == XIAOTAI_OK && stats(p).pool_bytes == 5529600);
    assert(p->close(p->self) == XIAOTAI_OK && csi_fake.start_calls == 1);
}

static char report_buf[850];

static void write_line(void *ctx, const char *line)
{
    assert(ctx == report_buf);
    assert(strlen(report_buf) + strlen(line) + 1 < sizeof(report_buf));
    strcat(report_buf, line);
    strcat(report_buf, "\n");
}

static void test_wt99(void)
{
    xiaotai_camera_csi_config_t cfg = base_config();
    cfg.sensor_id = 0xcb3a;
    cfg.sensor_id_reg = 0x3107;
    cfg.sensor_address = 0x30;
    cfg.sccb_hz = 400000;
    cfg.width = 1024;
    cfg.height = 600;
    cfg.full_range = cfg.fixed_fps = cfg.keep_powered = true;
    const xiaotai_camera_t *p = create(&cfg);
    assert(p->desc.flags == XIAOTAI_CAMERA_FULL_RANGE && p->desc.row_align == 16);
    assert(p->prepare(p->self) == XIAOTAI_ERR_NOT_SUPPORTED && csi_fake.powered && csi_fake.start_calls == 1);
    assert(csi_fake.hz == 400000 && power_on == 1 && power_off == 1);
    csi_fake.reg[0x3107] = 0xcb;
    csi_fake.reg[0x3108] = 0x3a;
    assert(p->set_power(p->self, true) == XIAOTAI_OK && csi_fake.start_calls == 1);
    assert(stats(p).pool_bytes == 2801664);
    report_buf[0] = '\0';
    xiaotai_camera_csi_report(p, write_line, report_buf);
    assert(csi_fake.quality_calls == 1 && !csi_fake.fd && !csi_fake.streaming);
    assert(strstr(report_buf, "cached=1 same_frame=0 exposure=456") && strstr(report_buf, "gain_menu=2048"));
    xiaotai_camera_mode_t m = mode(p);
    assert(p->open(p->self, &m) == XIAOTAI_OK && csi_fake.full_range && csi_fake.rate_calls == 0);
    xiaotai_camera_frame_t first = {0}, second = {0};
    assert(p->borrow(p->self, NULL) == XIAOTAI_ERR_INVALID_ARG);
    unsigned returns = csi_fake.returns;
    assert(p->borrow(p->self, &first) == XIAOTAI_ERR_NOT_FOUND && csi_fake.dequeues == 1);
    const size_t visible = 1024U * 600U * 3U / 2U, padded = 1024U * 608U * 3U / 2U, pair = 1024U * 3U;
    uint8_t *data = csi_fake.pool[1];
    assert(csi_fake.capacities[1] == padded);
    memset(data, 0xa5, padded);
    for (size_t i = 0; i < pair; ++i) data[visible - pair + i] = (uint8_t)i;
    const camera_csi_buffer_t newest = {.index = 1, .sequence = 7, .captured_us = 99};
    push((camera_csi_buffer_t){.index = 0, .bytes = visible}, &newest);
    assert(p->borrow(p->self, &first) == XIAOTAI_OK && csi_fake.returns == returns + 1);
    assert(first.data == data && first.width == 1024 && first.height == 600 && first.rows == 608);
    assert(first.size == padded && first.sequence == 7 && first.captured_us == 99 && first.skipped == 1);
    for (size_t offset = visible; offset < padded; offset += pair) {
        assert(memcmp(data + offset, data + visible - pair, pair) == 0);
    }
    assert(p->borrow(p->self, &second) == XIAOTAI_ERR_INVALID_STATE);
    assert(p->close(p->self) == XIAOTAI_ERR_INVALID_STATE && p->set_power(p->self, false) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_camera_frame_t copied = first;
    assert(p->release(p->self, &copied) == XIAOTAI_ERR_INVALID_STATE && csi_fake.returns == returns + 1);
    assert(p->release(p->self, &first) == XIAOTAI_OK && !first.data && csi_fake.returns == returns + 2);
    push((camera_csi_buffer_t){.index = 2, .bytes = visible}, NULL);
    assert(p->borrow(p->self, &second) == XIAOTAI_OK && second.data == csi_fake.pool[2]);
    assert(p->release(p->self, &copied) == XIAOTAI_ERR_INVALID_STATE && csi_fake.returns == returns + 2);
    assert(second.data == csi_fake.pool[2] && !csi_fake.queued[2]);
    assert(p->release(p->self, &second) == XIAOTAI_OK && csi_fake.returns == returns + 3 && csi_fake.queued[2]);
    uint8_t *rejected = csi_fake.pool[2];
    rejected[visible] = 0x6a;
    for (unsigned i = 0; i < 2; ++i) {
        push((camera_csi_buffer_t){.index = 2, .bytes = i ? padded + 1 : visible - 1}, NULL);
        assert(p->borrow(p->self, &first) == XIAOTAI_ERR_INVALID_SIZE && !first.data && csi_fake.queued[2]);
        assert(rejected[visible] == 0x6a);
    }
    const camera_csi_buffer_t bad = {.index = 1, .bytes = visible - 1};
    push((camera_csi_buffer_t){.index = 0, .bytes = visible}, &bad);
    assert(p->borrow(p->self, &first) == XIAOTAI_ERR_INVALID_SIZE && csi_fake.queued[0] && csi_fake.queued[1]);
    push((camera_csi_buffer_t){.index = 0, .error = true}, NULL);
    assert(p->borrow(p->self, &first) == XIAOTAI_FAIL && !first.data && csi_fake.queued[0]);
    assert(stats(p).bad_frames == 4 && stats(p).stale_frames == 2);
    memset(data + visible, 0xa5, padded - visible);
    push((camera_csi_buffer_t){.index = 1, .bytes = padded}, NULL);
    assert(p->borrow(p->self, &first) == XIAOTAI_OK);
    assert(memcmp(first.data + visible, data + visible - pair, pair) == 0);
    assert(p->release(p->self, &first) == XIAOTAI_OK);
    assert(p->close(p->self) == XIAOTAI_OK && p->set_power(p->self, false) == XIAOTAI_OK);
    assert(csi_fake.powered && power_on == 1 && power_off == 1 && csi_fake.start_calls == 1);
}

static void test_wt99_already_aligned(void)
{
    xiaotai_camera_csi_config_t cfg = base_config();
    cfg.width = 1024;
    cfg.height = 608;
    const xiaotai_camera_t *p = create(&cfg);
    assert(p->set_power(p->self, true) == XIAOTAI_OK);
    const xiaotai_camera_mode_t wanted = mode(p);
    assert(p->open(p->self, &wanted) == XIAOTAI_OK);
    const size_t size = 1024U * 608U * 3U / 2U, pair = 1024U * 3U;
    uint8_t *data = csi_fake.pool[0];
    assert(csi_fake.capacities[0] == size);
    memset(data + size - 2 * pair, 0x3d, pair);
    memset(data + size - pair, 0xa7, pair);
    push((camera_csi_buffer_t){.index = 0, .bytes = size}, NULL);
    xiaotai_camera_frame_t frame = {0};
    assert(p->borrow(p->self, &frame) == XIAOTAI_OK);
    assert(frame.data == data && frame.width == 1024 && frame.height == 608 && frame.rows == 608);
    assert(frame.size == size);
    for (size_t i = 0; i < pair; ++i) {
        assert(data[size - 2 * pair + i] == 0x3d && data[size - pair + i] == 0xa7);
    }
    assert(p->release(p->self, &frame) == XIAOTAI_OK && p->close(p->self) == XIAOTAI_OK);
}

static void test_recovery(void)
{
    const xiaotai_camera_csi_config_t cfg = base_config();
    const xiaotai_camera_t *p = create(&cfg);
    xiaotai_camera_mode_t m = mode(p);
    assert(p->set_power(p->self, true) == XIAOTAI_OK);
    const csi_fake_call_t stages[] = {CSI_OPEN, CSI_FORMAT, CSI_REQUEST, CSI_QUERY, CSI_QBUF, CSI_STREAM_ON};
    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); ++i) {
        fail(stages[i], XIAOTAI_ERR_TIMEOUT);
        assert(p->open(p->self, &m) == XIAOTAI_ERR_TIMEOUT && !csi_fake.fd);
        assert(p->close(p->self) == XIAOTAI_OK);
    }
    fail(CSI_RATE, XIAOTAI_OK);
    csi_fake.reduced_count = 2;
    assert(p->open(p->self, &m) == XIAOTAI_ERR_NO_MEM && !csi_fake.fd);
    csi_fake.reduced_count = 0;
    csi_fake.query_bytes = csi_fake.format_bytes + 1U;
    assert(p->open(p->self, &m) == XIAOTAI_ERR_INVALID_SIZE && !csi_fake.fd);
    csi_fake.query_bytes = 0;
    assert(p->open(p->self, &m) == XIAOTAI_OK);
    xiaotai_camera_frame_t frame = {0};
    fail(CSI_DQBUF, XIAOTAI_ERR_TIMEOUT);
    assert(p->borrow(p->self, &frame) == XIAOTAI_ERR_TIMEOUT && !frame.data);
    fail(CSI_DQBUF, XIAOTAI_OK);
    for (unsigned i = 0; i < 5; ++i) {
        csi_fake.frames[i] = (camera_csi_buffer_t){.index = i % 3, .sequence = i + 1, .bytes = csi_fake.format_bytes};
    }
    csi_fake.frame_count = 5;
    unsigned dequeues = csi_fake.dequeues;
    assert(p->borrow(p->self, &frame) == XIAOTAI_OK && csi_fake.dequeues == dequeues + 3);
    assert(frame.sequence == 3 && frame.skipped == 2);
    fail(CSI_QBUF, XIAOTAI_ERR_TIMEOUT);
    assert(p->release(p->self, &frame) == XIAOTAI_ERR_TIMEOUT && !frame.data);
    assert(p->borrow(p->self, &frame) == XIAOTAI_ERR_INVALID_STATE);
    fail(CSI_STREAM_OFF, XIAOTAI_ERR_TIMEOUT);
    assert(p->close(p->self) == XIAOTAI_ERR_TIMEOUT && csi_fake.streaming && csi_fake.fd);
    assert(csi_fake.calls[csi_fake.call_count - 1] == CSI_STREAM_OFF);
    assert(p->set_power(p->self, false) == XIAOTAI_ERR_INVALID_STATE && csi_fake.powered);
    fail(CSI_RATE, XIAOTAI_OK);
    assert(p->close(p->self) == XIAOTAI_OK && !csi_fake.fd);
    assert(p->open(p->self, &m) == XIAOTAI_OK);
    push((camera_csi_buffer_t){.index = 99}, NULL);
    assert(p->borrow(p->self, &frame) == XIAOTAI_FAIL);
    csi_fake.fail_call = CSI_CLOSE;
    csi_fake.failure = XIAOTAI_ERR_TIMEOUT;
    assert(p->close(p->self) == XIAOTAI_ERR_TIMEOUT && !csi_fake.fd);
    unsigned closes = csi_fake.closes;
    assert(p->close(p->self) == XIAOTAI_ERR_TIMEOUT && csi_fake.closes == closes);
    assert(p->set_power(p->self, false) == XIAOTAI_ERR_TIMEOUT && csi_fake.powered);
    assert(p->open(p->self, &m) == XIAOTAI_ERR_TIMEOUT && !csi_fake.fd);
}

static void test_first_open_failure_report(void)
{
    const struct { csi_fake_call_t operation; const char *stage; int slot; } cases[] = {
        {CSI_OPEN, "open", -1}, {CSI_REQUEST, "reqbufs", -1}, {CSI_QUERY, "querybuf", 0},
        {CSI_QBUF, "qbuf", 0}, {CSI_STREAM_ON, "streamon", -1},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const xiaotai_camera_csi_config_t cfg = base_config();
        csi_fake.cache_alignment = 32U;
        expected_alignment = 64U;
        const xiaotai_camera_t *p = create(&cfg);
        xiaotai_camera_mode_t m = mode(p);
        assert(p->set_power(p->self, true) == XIAOTAI_OK && aligned_allocations == 3);
        fail(cases[i].operation, XIAOTAI_ERR_INVALID_ARG);
        csi_fake.native_errno = EINVAL;
        assert(p->open(p->self, &m) == XIAOTAI_ERR_INVALID_ARG && !csi_fake.fd);
        fail(CSI_FORMAT, XIAOTAI_FAIL);
        assert(p->open(p->self, &m) == XIAOTAI_FAIL && !csi_fake.fd);
        char expected[148];
        snprintf(expected, sizeof(expected), "first_open_failure=%s error=0x%x errno=%d slot=%d"
                 " userptr_alignment=64 userptr_residue=0", cases[i].stage,
                 (unsigned)XIAOTAI_ERR_INVALID_ARG, EINVAL, cases[i].slot);
        report_buf[0] = '\0';
        xiaotai_camera_csi_report(p, write_line, report_buf);
        assert(strstr(report_buf, expected));
    }
}

int main(void)
{
    const xiaotai_i2c_config_t cfg = {.port = 0, .sda_pin = 2, .scl_pin = 4, .hz = 400000};
    assert(xiaotai_i2c_create(&cfg, &bus) == XIAOTAI_OK);
    test_validation_and_pool();
    test_waveshare();
    test_wt99();
    test_wt99_already_aligned();
    test_recovery();
    test_first_open_failure_report();
}
