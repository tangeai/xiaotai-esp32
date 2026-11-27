// SPDX-License-Identifier: MIT
// Replaces legacy: tests/test_board_rollback.py
// Replaces legacy: tests/test_hardware_boundaries.py
#include "xiaotai/parts/power.h"
#include "xiaotai/port/os.h"
#include "xiaotai_bus_host.h"
#include "xiaotai_mem_host.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

enum { PORT = 0, PCA = 0x19, AW = 0x58, AXP = 0x34, IRQ = 0x49 };
static xiaotai_i2c_bus_t *bus;
static xiaotai_pca9557_t *pca;
static xiaotai_aw9523_t *aw;
static xiaotai_axp2101_t *axp;
static xiaotai_line_t pin[2], awpin[2], rail[2];
static xiaotai_bus_host_transfer_t logs[16];

static void write_is(unsigned i, uint8_t reg, uint8_t value, xiaotai_err_t result)
{
    assert(i < sizeof(logs) / sizeof(logs[0]) && logs[i].op == XIAOTAI_BUS_HOST_WRITE &&
           logs[i].reg == reg && logs[i].size == 1 && logs[i].data[0] == value && logs[i].result == result);
}

typedef struct {
    const xiaotai_line_t *get;
    char text[1664];
    unsigned lines;
} report_t;

static void capture(void *arg, const char *line)
{
    report_t *r = arg;
    ++r->lines;
    (void)snprintf(r->text, sizeof(r->text), "%s", line);
    if (r->get) {
        bool level;
        assert(xiaotai_line_get(r->get, &level) == XIAOTAI_OK); /* Writer runs outside chip lock. */
    }
}

typedef struct { const xiaotai_line_t *line; xiaotai_err_t result; } set_t;
static void *set_on(void *arg)
{
    set_t *s = arg;
    s->result = xiaotai_line_set(s->line, true);
    return NULL;
}

typedef struct { xiaotai_signal_t entered, release; } hold_t;

static xiaotai_err_t stall(void *arg, const xiaotai_bus_host_transfer_t *t, bool *apply)
{
    (void)apply;
    if (t->address == PCA && t->op == XIAOTAI_BUS_HOST_WRITE && t->reg == 1) {
        hold_t *h = arg;
        xiaotai_signal_give(&h->entered);
        assert(xiaotai_signal_take(&h->release, 2000));
    }
    return XIAOTAI_OK;
}

static void test_pca(void)
{
    const xiaotai_pca9557_config_t config = {.bus = bus, .address = PCA, .outputs = 3, .initial = 2,
                                           .lock_timeout_ms = 5};
    xiaotai_mem_host_clear();
    xiaotai_bus_host_i2c_poke(PORT, PCA, 3, 0xff);
    xiaotai_bus_host_i2c_fail(PORT, PCA, XIAOTAI_BUS_HOST_WRITE, 1, XIAOTAI_FAIL);
    assert(xiaotai_pca9557_create(&config, &pca) == XIAOTAI_FAIL && !pca);
    assert(xiaotai_bus_host_i2c_log(logs, 16) == 1);
    write_is(0, 1, 2, XIAOTAI_FAIL);
    xiaotai_bus_host_i2c_log_clear();
    xiaotai_bus_host_i2c_fail(PORT, PCA, XIAOTAI_BUS_HOST_WRITE, 2, XIAOTAI_FAIL);
    assert(xiaotai_pca9557_create(&config, &pca) == XIAOTAI_FAIL && !pca);
    assert(xiaotai_bus_host_i2c_peek(PORT, PCA, 3) == 0xff && xiaotai_mem_host_outstanding() == 0);
    assert(xiaotai_bus_host_i2c_log(logs, 16) == 2);
    write_is(0, 1, 2, XIAOTAI_OK);
    write_is(1, 3, 0xfc, XIAOTAI_FAIL);
    xiaotai_bus_host_i2c_log_clear();
    assert(xiaotai_pca9557_create(&config, &pca) == XIAOTAI_OK);
    assert(xiaotai_bus_host_i2c_log(logs, 16) == 2);
    write_is(0, 1, 2, XIAOTAI_OK);
    write_is(1, 3, 0xfc, XIAOTAI_OK);
    assert(xiaotai_pca9557_line(pca, 0, false, &pin[0]) == XIAOTAI_OK);
    assert(xiaotai_pca9557_line(pca, 1, true, &pin[1]) == XIAOTAI_OK);
    xiaotai_line_t invalid = pin[0];
    assert(xiaotai_pca9557_line(pca, 2, false, &invalid) == XIAOTAI_ERR_INVALID_ARG &&
           !xiaotai_line_connected(&invalid));
    xiaotai_bus_host_i2c_fail(PORT, PCA, XIAOTAI_BUS_HOST_WRITE, 1, XIAOTAI_FAIL);
    assert(xiaotai_line_set(&pin[0], true) == XIAOTAI_FAIL);
    assert(xiaotai_line_set(&pin[1], true) == XIAOTAI_OK);
    assert(xiaotai_bus_host_i2c_peek(PORT, PCA, 1) == 0);

    hold_t hold;
    assert(xiaotai_signal_init(&hold.entered) == XIAOTAI_OK && xiaotai_signal_init(&hold.release) == XIAOTAI_OK);
    xiaotai_bus_host_i2c_hook(PORT, stall, &hold);
    pthread_t thread;
    set_t job = {.line = &pin[0], .result = XIAOTAI_FAIL};
    assert(pthread_create(&thread, NULL, set_on, &job) == 0);
    assert(xiaotai_signal_take(&hold.entered, 2000));
    assert(xiaotai_line_set(&pin[1], false) == XIAOTAI_ERR_TIMEOUT);
    xiaotai_signal_give(&hold.release);
    assert(pthread_join(thread, NULL) == 0 && job.result == XIAOTAI_OK);
    xiaotai_bus_host_i2c_hook(PORT, NULL, NULL);
    xiaotai_signal_deinit(&hold.release);
    xiaotai_signal_deinit(&hold.entered);
    assert(xiaotai_bus_host_i2c_peek(PORT, PCA, 1) == 1);
    xiaotai_bus_host_i2c_fail(PORT, PCA, XIAOTAI_BUS_HOST_READ, 1, XIAOTAI_FAIL);
    report_t rpt = {.get = &pin[0]};
    xiaotai_pca9557_report(pca, capture, &rpt);
    assert(rpt.lines == 1 && strstr(rpt.text, "PCA9557 addr=0x19 00=?? 01=01 03=fc"));
}

static void parallel(const xiaotai_line_t *a, const xiaotai_line_t *b)
{
    set_t left = {.line = a, .result = XIAOTAI_FAIL}, right = {.line = b, .result = XIAOTAI_FAIL};
    pthread_t t[2];
    assert(pthread_create(&t[0], NULL, set_on, &left) == 0);
    assert(pthread_create(&t[1], NULL, set_on, &right) == 0);
    assert(pthread_join(t[0], NULL) == 0 && pthread_join(t[1], NULL) == 0);
    assert(left.result == XIAOTAI_OK && right.result == XIAOTAI_OK);
}

static void test_aw(void)
{
    const xiaotai_reg_value_t setup[] = {{0x11, 0x10}, {0x03, 0x80}, {0x05, 0x0c}};
    const xiaotai_aw9523_config_t config = {.bus = bus, .address = AW, .setup = setup, .setup_count = 3};
    xiaotai_bus_host_i2c_log_clear();
    xiaotai_mem_host_clear();
    xiaotai_bus_host_i2c_fail(PORT, AW, XIAOTAI_BUS_HOST_WRITE, 2, XIAOTAI_ERR_NOT_FOUND);
    assert(xiaotai_aw9523_create(&config, &aw) == XIAOTAI_ERR_NOT_FOUND && !aw);
    assert(xiaotai_mem_host_outstanding() == 0 && xiaotai_bus_host_i2c_log(logs, 16) == 2);
    write_is(0, 0x11, 0x10, XIAOTAI_OK);
    write_is(1, 0x03, 0x80, XIAOTAI_ERR_NOT_FOUND);
    xiaotai_bus_host_i2c_log_clear();
    assert(xiaotai_aw9523_create(&config, &aw) == XIAOTAI_OK);
    assert(xiaotai_bus_host_i2c_log(logs, 16) == 3);
    for (unsigned i = 0; i < 3; ++i) write_is(i, setup[i].reg, setup[i].value, XIAOTAI_OK);
    assert(xiaotai_aw9523_line(aw, 1, 0, false, &awpin[0]) == XIAOTAI_OK);
    assert(xiaotai_aw9523_line(aw, 1, 1, false, &awpin[1]) == XIAOTAI_OK);
    xiaotai_bus_host_i2c_log_clear();
    parallel(&awpin[0], &awpin[1]);
    assert(xiaotai_bus_host_i2c_peek(PORT, AW, 3) == 0x83 && xiaotai_bus_host_i2c_log(logs, 16) == 4);
    assert(logs[0].op == XIAOTAI_BUS_HOST_READ && logs[1].op == XIAOTAI_BUS_HOST_WRITE &&
           logs[2].op == XIAOTAI_BUS_HOST_READ && logs[3].op == XIAOTAI_BUS_HOST_WRITE);
    assert(logs[0].thread == logs[1].thread && logs[2].thread == logs[3].thread && logs[0].thread != logs[2].thread);
    xiaotai_bus_host_i2c_fail(PORT, AW, XIAOTAI_BUS_HOST_WRITE, 1, XIAOTAI_FAIL);
    assert(xiaotai_line_set(&awpin[0], false) == XIAOTAI_FAIL && xiaotai_bus_host_i2c_peek(PORT, AW, 3) == 0x83);
    report_t rpt = {0};
    xiaotai_aw9523_report(aw, capture, &rpt);
    assert(rpt.lines == 1 && strstr(rpt.text, "AW9523 addr=0x58 03=83 05=0c 11=10"));
}

static xiaotai_err_t w1c(void *arg, const xiaotai_bus_host_transfer_t *t, bool *apply)
{
    (void)arg;
    if (t->address == AXP && t->reg == IRQ && t->op == XIAOTAI_BUS_HOST_WRITE) {
        assert(!(t->data[0] & ~0x0c));
        xiaotai_bus_host_i2c_poke(PORT, AXP, IRQ, xiaotai_bus_host_i2c_peek(PORT, AXP, IRQ) & (uint8_t)~t->data[0]);
        *apply = false;
    }
    return XIAOTAI_OK;
}

static void key_period(const xiaotai_input_t *key, uint32_t expected)
{
    const uint32_t prev = xiaotai_bus_host_i2c_transfers();
    for (unsigned i = 0; i < 4U; ++i) assert(key->poll(key->self) == 0);
    assert(xiaotai_bus_host_i2c_transfers() == prev);
    assert(key->poll(key->self) == expected);
}

static void test_axp(void)
{
    const xiaotai_reg_value_t setup[] = {{0x92, 13}, {0x90, 0x80}, {0x27, 0}};
    xiaotai_bus_host_i2c_log_clear();
    assert(xiaotai_axp2101_create(&(xiaotai_axp2101_config_t){.bus = bus, .address = AXP,
        .setup = setup, .setup_count = 3}, &axp) == XIAOTAI_OK);
    assert(xiaotai_bus_host_i2c_log(logs, 16) == 3);
    for (unsigned i = 0; i < 3; ++i) write_is(i, setup[i].reg, setup[i].value, XIAOTAI_OK);
    assert(xiaotai_axp2101_line(axp, &(xiaotai_axp2101_rail_t){0x90, 0x14, 0x94, 28}, &rail[0]) == XIAOTAI_OK);
    assert(xiaotai_axp2101_line(axp, &(xiaotai_axp2101_rail_t){0x90, 0x20, 0, 0}, &rail[1]) == XIAOTAI_OK);
    xiaotai_bus_host_i2c_log_clear();
    xiaotai_bus_host_i2c_fail(PORT, AXP, XIAOTAI_BUS_HOST_WRITE, 1, XIAOTAI_FAIL);
    assert(xiaotai_line_set(&rail[0], true) == XIAOTAI_FAIL && xiaotai_bus_host_i2c_log(logs, 16) == 1);
    write_is(0, 0x94, 28, XIAOTAI_FAIL);
    xiaotai_bus_host_i2c_log_clear();
    parallel(&rail[0], &rail[1]);
    assert(xiaotai_bus_host_i2c_log(logs, 16) == 5 && xiaotai_bus_host_i2c_peek(PORT, AXP, 0x90) == 0xb4);
    const unsigned voltage = logs[0].reg == 0x94 ? 0U : 2U;
    write_is(voltage, 0x94, 28, XIAOTAI_OK);
    assert(logs[voltage + 1U].reg == 0x90 && logs[voltage + 1U].op == XIAOTAI_BUS_HOST_READ &&
           logs[voltage + 2U].reg == 0x90 && logs[voltage + 2U].op == XIAOTAI_BUS_HOST_WRITE);
    assert(xiaotai_line_set(&rail[0], false) == XIAOTAI_OK && xiaotai_bus_host_i2c_peek(PORT, AXP, 0x90) == 0xa0);

    const xiaotai_input_t *key;
    assert(xiaotai_axp2101_power_key(axp, "main", &key) == XIAOTAI_OK && key->desc.events == XIAOTAI_INPUT_CLICK);
    xiaotai_bus_host_i2c_hook(PORT, w1c, NULL);
    xiaotai_bus_host_i2c_poke(PORT, AXP, IRQ, 0x8c);
    assert(key->open(key->self) == XIAOTAI_OK && xiaotai_bus_host_i2c_peek(PORT, AXP, IRQ) == 0x80);
    key_period(key, 0);
    xiaotai_bus_host_i2c_poke(PORT, AXP, IRQ, 0x84);
    key_period(key, 0); /* Long event is cleared; no short click. */
    assert(xiaotai_bus_host_i2c_peek(PORT, AXP, IRQ) == 0x80);
    xiaotai_bus_host_i2c_poke(PORT, AXP, IRQ, 0x88);
    xiaotai_bus_host_i2c_fail(PORT, AXP, XIAOTAI_BUS_HOST_READ, 1, XIAOTAI_FAIL);
    key_period(key, 0);
    xiaotai_bus_host_i2c_fail(PORT, AXP, XIAOTAI_BUS_HOST_WRITE, 1, XIAOTAI_FAIL);
    key_period(key, 0);
    assert(xiaotai_bus_host_i2c_peek(PORT, AXP, IRQ) == 0x88);
    key_period(key, XIAOTAI_INPUT_CLICK);
    key_period(key, 0);
    report_t rpt = {0};
    xiaotai_axp2101_report(axp, capture, &rpt);
    assert(rpt.lines == 1 && strstr(rpt.text, "AXP2101 addr=0x34") && strstr(rpt.text, "49=80") &&
           strstr(rpt.text, "90=a0") && strstr(rpt.text, "94=1c") && strstr(rpt.text, "key_errors=2"));
}

int main(void)
{
    assert(xiaotai_i2c_create(&(xiaotai_i2c_config_t){.port = PORT, .sda_pin = 1, .scl_pin = 2, .hz = 400000},
                              &bus) == XIAOTAI_OK);
    assert(xiaotai_bus_host_i2c_attach(PORT, PCA) == XIAOTAI_OK && xiaotai_bus_host_i2c_attach(PORT, AW) == XIAOTAI_OK &&
           xiaotai_bus_host_i2c_attach(PORT, AXP) == XIAOTAI_OK);
    test_pca();
    test_aw();
    test_axp();
    assert(xiaotai_bus_host_i2c_overlaps() == 0);
    return 0;
}
