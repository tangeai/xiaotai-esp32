// SPDX-License-Identifier: MIT
/* Bus component on the host fakes: bus creation and the controller rule,
 * transfers and their argument checks, the read-modify-write done in one bus
 * hold (deterministically with a held bus, and under contention), a failed
 * write leaving the register as it was, the bus-wait timeout, zeroed lines,
 * pin lines with their polarity and initial level, and levels on PWM and on
 * lines including out-of-range values. */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "xiaotai/parts/bus.h"
#include "xiaotai/port/os.h"
#include "xiaotai_bus_host.h"
#include "xiaotai_mem_host.h"

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

#define RUN(test)                                         \
    do {                                                  \
        xiaotai_bus_host_reset();                         \
        xiaotai_mem_host_clear();                         \
        if (test() != 0) {                                \
            fprintf(stderr, "FAILED: %s\n", #test);       \
            return 1;                                     \
        }                                                 \
    } while (0)

static xiaotai_i2c_config_t bus_config(int port, uint16_t timeout_ms)
{
    return (xiaotai_i2c_config_t){
        .port = port, .sda_pin = 7, .scl_pin = 8, .hz = 400000, .pullup = true, .timeout_ms = timeout_ms,
    };
}

static int64_t elapsed_ms(int64_t since_us)
{
    return (xiaotai_time_us() - since_us) / 1000;
}

/* ------------------------------------------------------------- creation */

static int test_create(void)
{
    xiaotai_i2c_bus_t *bus = (xiaotai_i2c_bus_t *)1;
    xiaotai_i2c_config_t config = {.port = 0, .sda_pin = 1, .scl_pin = 2, .hz = 100000, .pullup = true};

    CHECK(xiaotai_i2c_create(NULL, &bus) == XIAOTAI_ERR_INVALID_ARG && bus == NULL);
    CHECK(xiaotai_i2c_create(&config, NULL) == XIAOTAI_ERR_INVALID_ARG);
    xiaotai_i2c_config_t bad = config;
    bad.hz = 0;
    CHECK(xiaotai_i2c_create(&bad, &bus) == XIAOTAI_ERR_INVALID_ARG);
    bad = config;
    bad.port = -1;
    CHECK(xiaotai_i2c_create(&bad, &bus) == XIAOTAI_ERR_INVALID_ARG);
    bad = config;
    bad.sda_pin = -1;
    CHECK(xiaotai_i2c_create(&bad, &bus) == XIAOTAI_ERR_INVALID_ARG);
    bad = config;
    bad.scl_pin = -1;
    CHECK(xiaotai_i2c_create(&bad, &bus) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_mem_host_allocations() == 0);
    CHECK(!xiaotai_bus_host_i2c_port(0, NULL));

    /* The controller gets the board's pins, speed and pull-ups; timeout 0
     * means the 100 ms every existing bus uses. */
    CHECK(xiaotai_i2c_create(&config, &bus) == XIAOTAI_OK && bus != NULL);
    xiaotai_i2c_config_t opened;
    CHECK(xiaotai_bus_host_i2c_port(0, &opened));
    CHECK(opened.sda_pin == 1 && opened.scl_pin == 2 && opened.hz == 100000 && opened.pullup);
    CHECK(opened.timeout_ms == 100);
    /* One object, kept for good. */
    CHECK(xiaotai_mem_host_outstanding() == 1);

    /* A controller already in use is refused and the attempt leaks nothing. */
    xiaotai_i2c_bus_t *again = NULL;
    CHECK(xiaotai_i2c_create(&config, &again) == XIAOTAI_ERR_INVALID_STATE && again == NULL);
    CHECK(xiaotai_mem_host_outstanding() == 1);

    /* Out of memory: nothing is opened, so the controller stays free. */
    xiaotai_i2c_config_t second = bus_config(1, 250);
    xiaotai_mem_host_fail_nth(1);
    CHECK(xiaotai_i2c_create(&second, &again) == XIAOTAI_ERR_NO_MEM && again == NULL);
    CHECK(!xiaotai_bus_host_i2c_port(1, NULL));
    CHECK(xiaotai_i2c_create(&second, &again) == XIAOTAI_OK);
    CHECK(xiaotai_bus_host_i2c_port(1, &opened) && opened.timeout_ms == 250 && opened.hz == 400000);

    /* The platform handle, for drivers on platform libraries: one per bus. */
    CHECK(xiaotai_i2c_native(bus) != NULL && xiaotai_i2c_native(again) != NULL);
    CHECK(xiaotai_i2c_native(bus) != xiaotai_i2c_native(again));
    CHECK(xiaotai_i2c_native(NULL) == NULL);
    return 0;
}

/* ------------------------------------------------------------ transfers */

static int test_transfers(void)
{
    xiaotai_i2c_config_t config = bus_config(0, 0);
    xiaotai_i2c_bus_t *bus = NULL;
    CHECK(xiaotai_i2c_create(&config, &bus) == XIAOTAI_OK);
    CHECK(xiaotai_bus_host_i2c_attach(0, 0x18) == XIAOTAI_OK);

    CHECK(xiaotai_i2c_probe(bus, 0x18) == XIAOTAI_OK);
    CHECK(xiaotai_i2c_probe(bus, 0x3c) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_i2c_probe(bus, 0x80) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_i2c_probe(NULL, 0x18) == XIAOTAI_ERR_INVALID_ARG);

    const uint8_t bytes[] = {0x11, 0x22, 0x33};
    CHECK(xiaotai_i2c_write(bus, 0x18, 0x10, bytes, sizeof(bytes)) == XIAOTAI_OK);
    CHECK(xiaotai_bus_host_i2c_peek(0, 0x18, 0x10) == 0x11);
    CHECK(xiaotai_bus_host_i2c_peek(0, 0x18, 0x11) == 0x22);
    CHECK(xiaotai_bus_host_i2c_peek(0, 0x18, 0x12) == 0x33);
    uint8_t read[3] = {0};
    CHECK(xiaotai_i2c_read(bus, 0x18, 0x10, read, sizeof(read)) == XIAOTAI_OK);
    CHECK(memcmp(read, bytes, sizeof(bytes)) == 0);
    /* Writing only the register pointer is a valid transfer. */
    CHECK(xiaotai_i2c_write(bus, 0x18, 0x20, NULL, 0) == XIAOTAI_OK);

    xiaotai_bus_host_transfer_t log[8];
    CHECK(xiaotai_bus_host_i2c_log(log, 8) == 5);
    CHECK(log[0].op == XIAOTAI_BUS_HOST_PROBE && log[0].result == XIAOTAI_OK);
    CHECK(log[1].op == XIAOTAI_BUS_HOST_PROBE && log[1].address == 0x3c && log[1].result == XIAOTAI_ERR_NOT_FOUND);
    CHECK(log[2].op == XIAOTAI_BUS_HOST_WRITE && log[2].address == 0x18 && log[2].reg == 0x10);
    CHECK(log[2].size == 3 && log[2].data[2] == 0x33 && log[2].timeout_ms == 100);
    CHECK(log[3].op == XIAOTAI_BUS_HOST_READ && log[3].reg == 0x10 && log[3].size == 3 && log[3].data[1] == 0x22);
    CHECK(log[4].op == XIAOTAI_BUS_HOST_WRITE && log[4].reg == 0x20 && log[4].size == 0);

    /* Bad arguments never reach the bus. */
    const uint32_t before = xiaotai_bus_host_i2c_transfers();
    CHECK(xiaotai_i2c_read(bus, 0x18, 0, read, 0) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_i2c_read(bus, 0x18, 0, NULL, 1) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_i2c_read(bus, 0x90, 0, read, 1) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_i2c_write(bus, 0x18, 0, NULL, 1) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_i2c_update(NULL, 0x18, 0, 1, 1) == XIAOTAI_ERR_INVALID_ARG);
    uint8_t large[33] = {0};
    CHECK(xiaotai_i2c_write(bus, 0x18, 0x40, large, 32) == XIAOTAI_OK);
    CHECK(xiaotai_i2c_write(bus, 0x18, 0x40, large, 33) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(xiaotai_bus_host_i2c_transfers() == before + 1);

    /* Results of the wire pass through: no acknowledge, a stretched device. */
    CHECK(xiaotai_i2c_read(bus, 0x3c, 0, read, 1) == XIAOTAI_ERR_NOT_FOUND);
    xiaotai_bus_host_i2c_fail(0, 0x18, XIAOTAI_BUS_HOST_READ, 1, XIAOTAI_ERR_TIMEOUT);
    CHECK(xiaotai_i2c_read(bus, 0x18, 0x10, read, 1) == XIAOTAI_ERR_TIMEOUT);
    CHECK(xiaotai_i2c_read(bus, 0x18, 0x10, read, 1) == XIAOTAI_OK && read[0] == 0x11);
    return 0;
}

/* --------------------------------------------------- read-modify-write */

static int test_update(void)
{
    xiaotai_i2c_config_t config = bus_config(0, 0);
    xiaotai_i2c_bus_t *bus = NULL;
    CHECK(xiaotai_i2c_create(&config, &bus) == XIAOTAI_OK);
    CHECK(xiaotai_bus_host_i2c_attach(0, 0x34) == XIAOTAI_OK);
    xiaotai_bus_host_i2c_poke(0, 0x34, 0x90, 0xf0);

    /* Only the masked bits change; bits outside the mask are ignored. */
    xiaotai_bus_host_i2c_log_clear();
    CHECK(xiaotai_i2c_update(bus, 0x34, 0x90, 0x0c, 0x04) == XIAOTAI_OK);
    CHECK(xiaotai_bus_host_i2c_peek(0, 0x34, 0x90) == 0xf4);
    CHECK(xiaotai_i2c_update(bus, 0x34, 0x90, 0x80, 0x7f) == XIAOTAI_OK);
    CHECK(xiaotai_bus_host_i2c_peek(0, 0x34, 0x90) == 0x74);
    xiaotai_bus_host_transfer_t log[8];
    CHECK(xiaotai_bus_host_i2c_log(log, 8) == 4);
    CHECK(log[0].op == XIAOTAI_BUS_HOST_READ && log[0].reg == 0x90 && log[0].size == 1);
    CHECK(log[1].op == XIAOTAI_BUS_HOST_WRITE && log[1].reg == 0x90 && log[1].size == 1 && log[1].data[0] == 0xf4);
    /* An unchanged value is still written, as the existing helper did. */
    CHECK(xiaotai_i2c_update(bus, 0x34, 0x90, 0x01, 0x00) == XIAOTAI_OK);
    CHECK(xiaotai_bus_host_i2c_transfers() == 6);

    /* A failed write leaves the register as it was and reports the error. */
    xiaotai_bus_host_i2c_fail(0, 0x34, XIAOTAI_BUS_HOST_WRITE, 1, XIAOTAI_FAIL);
    CHECK(xiaotai_i2c_update(bus, 0x34, 0x90, 0x01, 0x01) == XIAOTAI_FAIL);
    CHECK(xiaotai_bus_host_i2c_peek(0, 0x34, 0x90) == 0x74);
    /* A failed read writes nothing. */
    xiaotai_bus_host_i2c_log_clear();
    xiaotai_bus_host_i2c_fail(0, 0x34, XIAOTAI_BUS_HOST_READ, 1, XIAOTAI_ERR_TIMEOUT);
    CHECK(xiaotai_i2c_update(bus, 0x34, 0x90, 0x01, 0x01) == XIAOTAI_ERR_TIMEOUT);
    CHECK(xiaotai_bus_host_i2c_transfers() == 1);
    CHECK(xiaotai_bus_host_i2c_peek(0, 0x34, 0x90) == 0x74);
    /* An absent chip: one unanswered read, no write. */
    xiaotai_bus_host_i2c_log_clear();
    CHECK(xiaotai_i2c_update(bus, 0x58, 0x03, 0x02, 0x02) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_bus_host_i2c_transfers() == 1);
    /* The bus is free again after every failure. */
    CHECK(xiaotai_i2c_update(bus, 0x34, 0x90, 0x01, 0x01) == XIAOTAI_OK);
    CHECK(xiaotai_bus_host_i2c_peek(0, 0x34, 0x90) == 0x75);
    return 0;
}

/* A hook that holds the bus inside the first read until released. */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool armed, inside, release;
} gate_t;

static gate_t s_gate = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, false, false, false};

static void gate_arm(void)
{
    pthread_mutex_lock(&s_gate.mutex);
    s_gate.armed = true;
    s_gate.inside = s_gate.release = false;
    pthread_mutex_unlock(&s_gate.mutex);
}

static xiaotai_err_t gate_hook(void *ctx, const xiaotai_bus_host_transfer_t *transfer, bool *apply)
{
    (void)ctx;
    (void)apply;
    pthread_mutex_lock(&s_gate.mutex);
    if (s_gate.armed && transfer->op == XIAOTAI_BUS_HOST_READ) {
        s_gate.armed = false;
        s_gate.inside = true;
        pthread_cond_broadcast(&s_gate.changed);
        struct timespec limit;
        clock_gettime(CLOCK_REALTIME, &limit);
        limit.tv_sec += 3;
        while (!s_gate.release) {
            if (pthread_cond_timedwait(&s_gate.changed, &s_gate.mutex, &limit) != 0) break;
        }
    }
    pthread_mutex_unlock(&s_gate.mutex);
    return XIAOTAI_OK;
}

static bool gate_wait_inside(void)
{
    struct timespec limit;
    clock_gettime(CLOCK_REALTIME, &limit);
    limit.tv_sec += 3;
    pthread_mutex_lock(&s_gate.mutex);
    while (!s_gate.inside) {
        if (pthread_cond_timedwait(&s_gate.changed, &s_gate.mutex, &limit) != 0) break;
    }
    const bool inside = s_gate.inside;
    pthread_mutex_unlock(&s_gate.mutex);
    return inside;
}

static void gate_open(void)
{
    pthread_mutex_lock(&s_gate.mutex);
    s_gate.release = true;
    pthread_cond_broadcast(&s_gate.changed);
    pthread_mutex_unlock(&s_gate.mutex);
}

typedef struct {
    xiaotai_i2c_bus_t *bus;
    uint8_t mask, bits;
    xiaotai_err_t result;
    uintptr_t thread;
    bool done;
} update_job_t;

static void *update_thread(void *arg)
{
    update_job_t *job = arg;
    job->thread = (uintptr_t)pthread_self();
    job->result = xiaotai_i2c_update(job->bus, 0x58, 0x03, job->mask, job->bits);
    __atomic_store_n(&job->done, true, __ATOMIC_SEQ_CST);
    return NULL;
}

static int test_update_one_hold(void)
{
    xiaotai_i2c_config_t config = bus_config(0, 1000);
    xiaotai_i2c_bus_t *bus = NULL;
    CHECK(xiaotai_i2c_create(&config, &bus) == XIAOTAI_OK);
    CHECK(xiaotai_bus_host_i2c_attach(0, 0x58) == XIAOTAI_OK);
    xiaotai_bus_host_i2c_hook(0, gate_hook, NULL);
    gate_arm();

    /* A has read the register and holds the bus; B wants another bit. */
    update_job_t a = {.bus = bus, .mask = 0x01, .bits = 0x01};
    update_job_t b = {.bus = bus, .mask = 0x02, .bits = 0x02};
    pthread_t thread_a, thread_b;
    CHECK(pthread_create(&thread_a, NULL, update_thread, &a) == 0);
    CHECK(gate_wait_inside());
    CHECK(pthread_create(&thread_b, NULL, update_thread, &b) == 0);
    xiaotai_delay_ms(100);
    /* B is waiting for the bus: it has not started a transfer between A's
     * read and A's write. */
    CHECK(!__atomic_load_n(&b.done, __ATOMIC_SEQ_CST));
    CHECK(xiaotai_bus_host_i2c_transfers() == 1);
    gate_open();
    CHECK(pthread_join(thread_a, NULL) == 0 && pthread_join(thread_b, NULL) == 0);
    CHECK(a.result == XIAOTAI_OK && b.result == XIAOTAI_OK);

    /* Neither update was lost, and each was read-then-write without the other
     * in between. */
    CHECK(xiaotai_bus_host_i2c_peek(0, 0x58, 0x03) == 0x03);
    xiaotai_bus_host_transfer_t log[4];
    CHECK(xiaotai_bus_host_i2c_log(log, 4) == 4);
    CHECK(log[0].op == XIAOTAI_BUS_HOST_READ && log[0].thread == a.thread);
    CHECK(log[1].op == XIAOTAI_BUS_HOST_WRITE && log[1].thread == a.thread && log[1].data[0] == 0x01);
    CHECK(log[2].op == XIAOTAI_BUS_HOST_READ && log[2].thread == b.thread);
    CHECK(log[3].op == XIAOTAI_BUS_HOST_WRITE && log[3].thread == b.thread && log[3].data[0] == 0x03);
    CHECK(xiaotai_bus_host_i2c_overlaps() == 0);
    return 0;
}

/* Two tasks flipping different bits of one register, many times, with the
 * fake yielding inside every transfer to invite interleaving. */
typedef struct {
    xiaotai_i2c_bus_t *bus;
    uint8_t bit;
    int failures;
} toggle_job_t;

#define TOGGLES 1000

static xiaotai_err_t yield_hook(void *ctx, const xiaotai_bus_host_transfer_t *transfer, bool *apply)
{
    (void)ctx;
    (void)transfer;
    (void)apply;
    sched_yield();
    return XIAOTAI_OK;
}

static void *toggle_thread(void *arg)
{
    toggle_job_t *job = arg;
    for (int i = 0; i < TOGGLES; ++i) {
        const uint8_t bits = (i & 1) ? job->bit : 0;
        if (xiaotai_i2c_update(job->bus, 0x20, 0x01, job->bit, bits) != XIAOTAI_OK) ++job->failures;
    }
    return NULL;
}

static int test_update_contention(void)
{
    xiaotai_i2c_config_t config = bus_config(2, 1000);
    xiaotai_i2c_bus_t *bus = NULL;
    CHECK(xiaotai_i2c_create(&config, &bus) == XIAOTAI_OK);
    CHECK(xiaotai_bus_host_i2c_attach(2, 0x20) == XIAOTAI_OK);
    xiaotai_bus_host_i2c_poke(2, 0x20, 0x01, 0x80);
    xiaotai_bus_host_i2c_hook(2, yield_hook, NULL);

    toggle_job_t first = {.bus = bus, .bit = 0x01}, second = {.bus = bus, .bit = 0x02};
    pthread_t a, b;
    CHECK(pthread_create(&a, NULL, toggle_thread, &first) == 0);
    CHECK(pthread_create(&b, NULL, toggle_thread, &second) == 0);
    CHECK(pthread_join(a, NULL) == 0 && pthread_join(b, NULL) == 0);
    CHECK(first.failures == 0 && second.failures == 0);

    /* The last toggle of each set its bit; the untouched bit survived. */
    CHECK(xiaotai_bus_host_i2c_peek(2, 0x20, 0x01) == 0x83);
    CHECK(xiaotai_bus_host_i2c_transfers() == 4 * TOGGLES);
    CHECK(xiaotai_bus_host_i2c_overlaps() == 0);
    /* Every read in the log is followed by the same thread's write. */
    static xiaotai_bus_host_transfer_t log[XIAOTAI_BUS_HOST_LOG];
    const size_t count = xiaotai_bus_host_i2c_log(log, XIAOTAI_BUS_HOST_LOG);
    CHECK(count == XIAOTAI_BUS_HOST_LOG);
    for (size_t i = 0; i + 1 < count; i += 2) {
        CHECK(log[i].op == XIAOTAI_BUS_HOST_READ && log[i + 1].op == XIAOTAI_BUS_HOST_WRITE);
        CHECK(log[i].thread == log[i + 1].thread);
    }
    return 0;
}

/* ------------------------------------------------ fake device behaviour */

/* Register 0x49 clears the bits written as one; register 0x00 refuses writes. */
static xiaotai_err_t clear_on_write_hook(void *ctx, const xiaotai_bus_host_transfer_t *transfer, bool *apply)
{
    (void)ctx;
    if (transfer->op != XIAOTAI_BUS_HOST_WRITE) return XIAOTAI_OK;
    if (transfer->reg == 0x00) return XIAOTAI_FAIL;
    if (transfer->reg == 0x49 && transfer->size == 1) {
        const uint8_t now = xiaotai_bus_host_i2c_peek(transfer->port, transfer->address, 0x49);
        xiaotai_bus_host_i2c_poke(transfer->port, transfer->address, 0x49, (uint8_t)(now & ~transfer->data[0]));
        *apply = false;
    }
    return XIAOTAI_OK;
}

static int test_fake_device(void)
{
    xiaotai_i2c_config_t config = bus_config(5, 0);
    xiaotai_i2c_bus_t *bus = NULL;
    CHECK(xiaotai_i2c_create(&config, &bus) == XIAOTAI_OK);
    CHECK(xiaotai_bus_host_i2c_attach(5, 0x34) == XIAOTAI_OK);
    xiaotai_bus_host_i2c_hook(5, clear_on_write_hook, NULL);
    xiaotai_bus_host_i2c_poke(5, 0x34, 0x49, 0x0c);
    xiaotai_bus_host_i2c_poke(5, 0x34, 0x00, 0x5a);

    const uint8_t clear = 0x04;
    CHECK(xiaotai_i2c_write(bus, 0x34, 0x49, &clear, 1) == XIAOTAI_OK);
    CHECK(xiaotai_bus_host_i2c_peek(5, 0x34, 0x49) == 0x08);
    uint8_t value = 0;
    CHECK(xiaotai_i2c_read(bus, 0x34, 0x49, &value, 1) == XIAOTAI_OK && value == 0x08);
    /* A hook error fails the transfer and stores nothing. */
    CHECK(xiaotai_i2c_update(bus, 0x34, 0x00, 0xff, 0x00) == XIAOTAI_FAIL);
    CHECK(xiaotai_bus_host_i2c_peek(5, 0x34, 0x00) == 0x5a);

    /* The nth matching transfer fails, not the ones before it. */
    xiaotai_bus_host_i2c_fail(5, 0x34, XIAOTAI_BUS_HOST_READ, 2, XIAOTAI_ERR_TIMEOUT);
    CHECK(xiaotai_i2c_read(bus, 0x34, 0x49, &value, 1) == XIAOTAI_OK);
    CHECK(xiaotai_i2c_read(bus, 0x34, 0x49, &value, 1) == XIAOTAI_ERR_TIMEOUT);
    CHECK(xiaotai_i2c_read(bus, 0x34, 0x49, &value, 1) == XIAOTAI_OK);

    /* A detached chip stops answering; attaching again starts from zero. */
    xiaotai_bus_host_i2c_detach(5, 0x34);
    CHECK(xiaotai_i2c_probe(bus, 0x34) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_bus_host_i2c_attach(5, 0x34) == XIAOTAI_OK);
    CHECK(xiaotai_i2c_read(bus, 0x34, 0x49, &value, 1) == XIAOTAI_OK && value == 0);
    /* Devices on another port are not this bus's. */
    CHECK(xiaotai_bus_host_i2c_attach(6, 0x77) == XIAOTAI_OK);
    CHECK(xiaotai_i2c_probe(bus, 0x77) == XIAOTAI_ERR_NOT_FOUND);
    return 0;
}

/* --------------------------------------------------------- bus waiting */

typedef struct {
    xiaotai_i2c_bus_t *bus;
    xiaotai_err_t result;
} read_job_t;

static void *read_thread(void *arg)
{
    read_job_t *job = arg;
    uint8_t value;
    job->result = xiaotai_i2c_read(job->bus, 0x18, 0x00, &value, 1);
    return NULL;
}

static int bus_wait_case(int port, uint16_t configured, int64_t expected_ms)
{
    xiaotai_i2c_config_t config = bus_config(port, configured);
    xiaotai_i2c_bus_t *bus = NULL;
    CHECK(xiaotai_i2c_create(&config, &bus) == XIAOTAI_OK);
    CHECK(xiaotai_bus_host_i2c_attach(port, 0x18) == XIAOTAI_OK);
    xiaotai_bus_host_i2c_hook(port, gate_hook, NULL);
    gate_arm();
    xiaotai_bus_host_i2c_log_clear();

    read_job_t holder = {.bus = bus};
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, read_thread, &holder) == 0);
    CHECK(gate_wait_inside());

    /* The bus stays busy past the timeout: the contender gives up with
     * TIMEOUT no earlier than the timeout and long before the holder ends,
     * without having touched the wire. */
    const int64_t start = xiaotai_time_us();
    uint8_t value;
    CHECK(xiaotai_i2c_read(bus, 0x18, 0x00, &value, 1) == XIAOTAI_ERR_TIMEOUT);
    const int64_t waited = elapsed_ms(start);
    CHECK(waited >= expected_ms);
    CHECK(waited < expected_ms + 900);
    CHECK(xiaotai_bus_host_i2c_transfers() == 1);
    gate_open();
    CHECK(pthread_join(thread, NULL) == 0 && holder.result == XIAOTAI_OK);
    CHECK(xiaotai_i2c_probe(bus, 0x18) == XIAOTAI_OK);
    return 0;
}

static int test_bus_wait(void)
{
    CHECK(bus_wait_case(3, 50, 50) == 0);
    /* 0 is the 100 ms default. */
    CHECK(bus_wait_case(4, 0, 100) == 0);
    return 0;
}

/* ----------------------------------------------------------------- lines */

static int test_zeroed_line(void)
{
    const xiaotai_line_t none = {0};
    bool active = true;
    CHECK(!xiaotai_line_connected(&none));
    CHECK(xiaotai_line_set(&none, true) == XIAOTAI_OK);
    CHECK(xiaotai_line_get(&none, &active) == XIAOTAI_OK && !active);
    active = true;
    CHECK(!xiaotai_line_connected(NULL));
    CHECK(xiaotai_line_set(NULL, true) == XIAOTAI_OK);
    CHECK(xiaotai_line_get(NULL, &active) == XIAOTAI_OK && !active);
    return 0;
}

static int test_pin_lines(void)
{
    /* Active high, starting inactive (an amplifier enable). */
    xiaotai_gpio_config_t amp = {.pin = 53};
    xiaotai_line_t line;
    CHECK(xiaotai_line_gpio_output(&amp, &line) == XIAOTAI_OK && xiaotai_line_connected(&line));
    xiaotai_bus_host_pin_t pin = xiaotai_bus_host_pin(53);
    CHECK(pin.configured && pin.output && !pin.level && !pin.level_when_output && !pin.open_drain);
    bool active = true;
    CHECK(xiaotai_line_get(&line, &active) == XIAOTAI_OK && !active);
    CHECK(xiaotai_line_set(&line, true) == XIAOTAI_OK && xiaotai_bus_host_pin(53).level);
    CHECK(xiaotai_line_get(&line, &active) == XIAOTAI_OK && active);
    CHECK(xiaotai_line_set(&line, false) == XIAOTAI_OK && !xiaotai_bus_host_pin(53).level);
    CHECK(xiaotai_bus_host_pin(53).sets == 2);

    /* Active low: the level is inverted, and the initial state is on the pin
     * before it drives. */
    xiaotai_gpio_config_t reset = {.pin = 27, .active_low = true, .initially_active = true, .open_drain = true,
                                   .pull_up = true};
    xiaotai_line_t reset_line;
    CHECK(xiaotai_line_gpio_output(&reset, &reset_line) == XIAOTAI_OK);
    pin = xiaotai_bus_host_pin(27);
    CHECK(pin.output && !pin.level_when_output && !pin.level && pin.open_drain && pin.pull_up && !pin.pull_down);
    CHECK(xiaotai_line_get(&reset_line, &active) == XIAOTAI_OK && active);
    CHECK(xiaotai_line_set(&reset_line, false) == XIAOTAI_OK && xiaotai_bus_host_pin(27).level);
    CHECK(xiaotai_line_get(&reset_line, &active) == XIAOTAI_OK && !active);
    xiaotai_gpio_config_t backlight = {.pin = 42, .active_low = true};
    CHECK(xiaotai_line_gpio_output(&backlight, &line) == XIAOTAI_OK);
    CHECK(xiaotai_bus_host_pin(42).level_when_output);

    /* An input: reads the pin through its polarity, refuses to be driven. */
    xiaotai_gpio_config_t key = {.pin = 35, .active_low = true, .pull_up = true};
    xiaotai_line_t key_line;
    CHECK(xiaotai_line_gpio_input(&key, &key_line) == XIAOTAI_OK && xiaotai_line_connected(&key_line));
    pin = xiaotai_bus_host_pin(35);
    CHECK(pin.configured && !pin.output && pin.pull_up);
    xiaotai_bus_host_pin_drive(35, false);
    CHECK(xiaotai_line_get(&key_line, &active) == XIAOTAI_OK && active);
    xiaotai_bus_host_pin_drive(35, true);
    CHECK(xiaotai_line_get(&key_line, &active) == XIAOTAI_OK && !active);
    CHECK(xiaotai_line_set(&key_line, true) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(xiaotai_bus_host_pin(35).sets == 0 && xiaotai_bus_host_pin(35).level);

    /* Lines carry their pin: making them allocates nothing. */
    CHECK(xiaotai_mem_host_allocations() == 0);
    return 0;
}

static xiaotai_err_t stale_get(void *self, bool *active)
{
    (void)self;
    *active = true;
    return XIAOTAI_OK;
}

static xiaotai_err_t stale_set(void *self, uint8_t percent)
{
    (void)self;
    (void)percent;
    return XIAOTAI_OK;
}

static int test_pin_line_failures(void)
{
    /* A refused line is left zeroed, whatever the caller's variable held. */
    xiaotai_line_t line = {.get = stale_get, .self = (void *)1};
    xiaotai_gpio_config_t bad = {.pin = -1};
    CHECK(xiaotai_line_gpio_output(&bad, &line) == XIAOTAI_ERR_INVALID_ARG && !xiaotai_line_connected(&line));
    CHECK(xiaotai_line_gpio_output(NULL, &line) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_line_gpio_input(&bad, &line) == XIAOTAI_ERR_INVALID_ARG && !xiaotai_line_connected(&line));

    /* Set-up refused by the platform: the line stays not connected. */
    xiaotai_gpio_config_t config = {.pin = 12};
    xiaotai_bus_host_pin_fail(12, XIAOTAI_FAIL);
    CHECK(xiaotai_line_gpio_output(&config, &line) == XIAOTAI_FAIL && !xiaotai_line_connected(&line));
    CHECK(!xiaotai_bus_host_pin(12).configured);

    /* Platform errors on set and get reach the caller; a failed get reads
     * inactive, a failed set changes nothing. */
    CHECK(xiaotai_line_gpio_output(&config, &line) == XIAOTAI_OK);
    xiaotai_bus_host_pin_fail(12, XIAOTAI_ERR_TIMEOUT);
    CHECK(xiaotai_line_set(&line, true) == XIAOTAI_ERR_TIMEOUT && !xiaotai_bus_host_pin(12).level);
    CHECK(xiaotai_line_set(&line, true) == XIAOTAI_OK);
    xiaotai_bus_host_pin_fail(12, XIAOTAI_FAIL);
    bool active = true;
    CHECK(xiaotai_line_get(&line, &active) == XIAOTAI_FAIL && !active);
    return 0;
}

/* ---------------------------------------------------------------- levels */

static int test_pwm_level(void)
{
    /* As the existing PWM backlights: 5 kHz, 10 bits, timer 1, channel 1. */
    const xiaotai_pwm_config_t config = {.pin = 20, .timer = 1, .channel = 1, .hz = 5000, .resolution_bits = 10};
    xiaotai_level_t level;
    CHECK(xiaotai_level_pwm(&config, &level) == XIAOTAI_OK && level.dimmable && level.set);
    xiaotai_bus_host_pwm_t pwm = xiaotai_bus_host_pwm(1);
    CHECK(pwm.configured && pwm.pin == 20 && pwm.timer == 1 && pwm.hz == 5000 && pwm.resolution_bits == 10);
    CHECK(pwm.duty == 0);
    CHECK(xiaotai_mem_host_outstanding() == 1);

    /* duty = 1023 * percent / 100, the existing backlight formula, for every
     * percent. */
    for (uint32_t percent = 0; percent <= 100; ++percent) {
        CHECK(level.set(level.self, (uint8_t)percent) == XIAOTAI_OK);
        CHECK(xiaotai_bus_host_pwm(1).duty == 1023U * percent / 100U);
    }
    CHECK(level.set(level.self, 50) == XIAOTAI_OK && xiaotai_bus_host_pwm(1).duty == 511);
    const uint32_t updates = xiaotai_bus_host_pwm(1).updates;

    /* Above 100 %: refused, nothing changes. */
    CHECK(level.set(level.self, 101) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(level.set(level.self, 255) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_bus_host_pwm(1).duty == 511 && xiaotai_bus_host_pwm(1).updates == updates);

    /* A platform failure reaches the caller and leaves the duty. */
    xiaotai_bus_host_pwm_fail(1, XIAOTAI_ERR_TIMEOUT);
    CHECK(level.set(level.self, 80) == XIAOTAI_ERR_TIMEOUT && xiaotai_bus_host_pwm(1).duty == 511);

    /* Active low: 0 % is a full-scale duty, so the output starts off. */
    const xiaotai_pwm_config_t inverted = {.pin = 21, .timer = 2, .channel = 2, .hz = 1000, .resolution_bits = 8,
                                           .active_low = true};
    xiaotai_level_t dim;
    CHECK(xiaotai_level_pwm(&inverted, &dim) == XIAOTAI_OK);
    /* The output inverts, not the duty: 0 % is inactive for the whole period. */
    CHECK(xiaotai_bus_host_pwm(2).active_low && xiaotai_bus_host_pwm(2).duty == 0 && !xiaotai_bus_host_pwm(1).active_low);
    CHECK(dim.set(dim.self, 100) == XIAOTAI_OK && xiaotai_bus_host_pwm(2).duty == 255);
    CHECK(dim.set(dim.self, 25) == XIAOTAI_OK && xiaotai_bus_host_pwm(2).duty == 63);
    return 0;
}

static int test_pwm_level_failures(void)
{
    xiaotai_level_t level = {.set = stale_set, .dimmable = true, .self = (void *)1};
    xiaotai_pwm_config_t config = {.pin = 20, .timer = 1, .channel = 3, .hz = 5000, .resolution_bits = 10};
    xiaotai_pwm_config_t bad = config;
    bad.resolution_bits = 0;
    CHECK(xiaotai_level_pwm(&bad, &level) == XIAOTAI_ERR_INVALID_ARG && level.set == NULL && !level.dimmable);
    bad.resolution_bits = 21;
    CHECK(xiaotai_level_pwm(&bad, &level) == XIAOTAI_ERR_INVALID_ARG);
    bad = config;
    bad.hz = 0;
    CHECK(xiaotai_level_pwm(&bad, &level) == XIAOTAI_ERR_INVALID_ARG);
    bad = config;
    bad.pin = -1;
    CHECK(xiaotai_level_pwm(&bad, &level) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_level_pwm(NULL, &level) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_mem_host_allocations() == 0 && !xiaotai_bus_host_pwm(3).configured);

    /* Out of memory: nothing configured. */
    xiaotai_mem_host_fail_nth(1);
    CHECK(xiaotai_level_pwm(&config, &level) == XIAOTAI_ERR_NO_MEM && level.set == NULL);
    CHECK(!xiaotai_bus_host_pwm(3).configured);

    /* The platform refuses the channel: the object is released. */
    xiaotai_mem_host_clear();
    xiaotai_bus_host_pwm_fail(3, XIAOTAI_FAIL);
    CHECK(xiaotai_level_pwm(&config, &level) == XIAOTAI_FAIL && level.set == NULL);
    CHECK(xiaotai_mem_host_allocations() == 1 && xiaotai_mem_host_outstanding() == 0);
    return 0;
}

static int test_line_level(void)
{
    /* An on/off backlight on an active-low pin, starting dark. */
    xiaotai_gpio_config_t pin = {.pin = 42, .active_low = true};
    static xiaotai_line_t line;
    CHECK(xiaotai_line_gpio_output(&pin, &line) == XIAOTAI_OK);
    xiaotai_level_t level = {.dimmable = true};
    xiaotai_level_from_line(&line, &level);
    CHECK(!level.dimmable && level.set);
    CHECK(xiaotai_bus_host_pin(42).level);
    CHECK(level.set(level.self, 1) == XIAOTAI_OK && !xiaotai_bus_host_pin(42).level);
    CHECK(level.set(level.self, 0) == XIAOTAI_OK && xiaotai_bus_host_pin(42).level);
    CHECK(level.set(level.self, 100) == XIAOTAI_OK && !xiaotai_bus_host_pin(42).level);
    const uint32_t sets = xiaotai_bus_host_pin(42).sets;
    CHECK(level.set(level.self, 101) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(!xiaotai_bus_host_pin(42).level && xiaotai_bus_host_pin(42).sets == sets);

    /* On a line that is not connected: checks its argument, does nothing. */
    static const xiaotai_line_t none = {0};
    xiaotai_level_from_line(&none, &level);
    CHECK(level.set(level.self, 50) == XIAOTAI_OK);
    CHECK(level.set(level.self, 101) == XIAOTAI_ERR_INVALID_ARG);
    xiaotai_level_from_line(NULL, &level);
    CHECK(level.set(level.self, 100) == XIAOTAI_OK);
    return 0;
}

int main(void)
{
    RUN(test_create);
    RUN(test_transfers);
    RUN(test_update);
    RUN(test_update_one_hold);
    RUN(test_update_contention);
    RUN(test_fake_device);
    RUN(test_bus_wait);
    RUN(test_zeroed_line);
    RUN(test_pin_lines);
    RUN(test_pin_line_failures);
    RUN(test_pwm_level);
    RUN(test_pwm_level_failures);
    RUN(test_line_level);
    printf("part_bus: all checks passed\n");
    return 0;
}
