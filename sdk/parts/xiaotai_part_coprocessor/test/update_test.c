// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_c61_ota_image_admission.py
#include "coprocessor_internal.h"
#include "xiaotai_coprocessor_host.h"
#include "xiaotai/port/mem.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *const projects[] = {"xiaotai_wt99p461_c61", "network_adapter", NULL};
static const char *const commands[] = {"INFO", "RXRESET", "SINK0", "SINK1", "ROLLBACK", NULL};
static const char *const restart[] = {"ROLLBACK", NULL};
static const xiaotai_coprocessor_config_t standard = {
    .chip_id = 20, .chip_revision = 100,
    .update_projects = projects, .update_image_max = 0x180000,
    .command_channel = 0x57545331, .commands = commands,
    .restart_commands = restart, .restart_command_wait_ms = 4000,
};

static const xiaotai_network_t *open(const xiaotai_coprocessor_config_t *cfg)
{
    const xiaotai_network_t *part;
    assert(xiaotai_coprocessor_create(cfg, &part) == XIAOTAI_OK && part);
    return part;
}

void __wrap_xiaotai_delay_ms(uint32_t ms) { xiaotai_coprocessor_host_note("delay %u", ms); }

static uint8_t bytes[1303];
static const size_t desc = 32;
static const char full_transfer[] =
    "chip\nbegin\nwrite +0 512\nwrite +512 512\nwrite +1024 279\nend\nactivate\ndelay 3000\n";

static void image(const char *project)
{
    memset(bytes, 0, sizeof(bytes));
    bytes[0] = 0xe9;
    bytes[1] = 1;
    bytes[12] = (uint8_t)standard.chip_id;
    bytes[15] = 99;
    bytes[17] = 101;
    bytes[29] = 1; /* first segment is 256 bytes */
    bytes[desc + 0] = 0x32;
    bytes[desc + 1] = 0x54;
    bytes[desc + 2] = 0xcd;
    bytes[desc + 3] = 0xab;
    memcpy(bytes + desc + 48, project, strlen(project));
}

static xiaotai_coprocessor_state_t state(const xiaotai_network_t *part)
{
    xiaotai_coprocessor_state_t out;
    xiaotai_coprocessor_state(part, &out);
    return out;
}

typedef struct {
    const xiaotai_network_t *part;
    xiaotai_network_stage_t last;
    uint8_t percent;
    unsigned calls;
} progress_t;

static void progress(xiaotai_network_stage_t stage, uint8_t percentage, void *ctx)
{
    progress_t *p = ctx;
    assert(state(p->part).stage == stage); /* callback can query diagnostics, outside lock */
    assert(percentage <= 100);
    if (stage == XIAOTAI_NETWORK_WRITING && p->last == stage) assert(percentage >= p->percent);
    p->last = stage;
    p->percent = percentage;
    ++p->calls;
}

static void boot_fake(void)
{
    xiaotai_coprocessor_host_reset();
    xiaotai_coprocessor_host()->chip = standard.chip_id;
    xiaotai_coprocessor_host()->image = bytes;
}

static void validation(void)
{
    boot_fake();
    const xiaotai_network_t *part = open(&standard);
    assert(part->coprocessor_update && part->coprocessor_command);
    for (int var = 0; var < 14; ++var) {
        image(projects[0]);
        size_t size = sizeof(bytes);
        if (var == 0) bytes[0] = 0;
        if (var == 1) bytes[1] = 0;
        if (var == 2) bytes[1] = 17;
        if (var == 3) bytes[12] = 13;
        if (var == 4) bytes[15] = 101;
        if (var == 5) bytes[17] = 99;
        if (var == 6) bytes[desc] = 0;
        if (var == 7) bytes[29] = 0;
        if (var == 8) bytes[29] = 6;
        if (var == 9) image("xiaotai_wt99p461_c61_extra");
        if (var == 10) memset(bytes + desc + 48, 'x', 32);
        if (var == 11) size = desc + 255;
        if (var == 12) { bytes[28] = 255; bytes[29] = 0; } /* One byte short of the app descriptor. */
        if (var == 13) memset(bytes + 28, 0xff, sizeof(uint32_t)); /* UINT32_MAX first-segment length. */
        progress_t p = {.part = part};
        const xiaotai_err_t err = part->coprocessor_update(part->self, bytes, size, progress, &p);
        assert(err == (var == 11 ? XIAOTAI_ERR_INVALID_SIZE : XIAOTAI_ERR_INVALID_VERSION));
        assert(p.calls == 2 && p.last == XIAOTAI_NETWORK_BAD_IMAGE && state(part).error == err);
        assert(!state(part).transfer_started && !state(part).attempt_recorded);
    }
    assert(part->coprocessor_update(part->self, bytes, standard.update_image_max + 1U, NULL, NULL) ==
           XIAOTAI_ERR_INVALID_SIZE);
    assert(xiaotai_coprocessor_host_journal()[0] == '\0');
    image(projects[0]);
    xiaotai_coprocessor_host()->chip = 13;
    assert(part->coprocessor_update(part->self, bytes, sizeof(bytes), NULL, NULL) == XIAOTAI_ERR_INVALID_RESPONSE);
    assert(state(part).stage == XIAOTAI_NETWORK_UNSUPPORTED && !state(part).transfer_started);
    xiaotai_coprocessor_host()->chip = 0;
    assert(part->coprocessor_update(part->self, bytes, sizeof(bytes), NULL, NULL) == XIAOTAI_ERR_INVALID_RESPONSE);
    assert(state(part).stage == XIAOTAI_NETWORK_NO_LINK && !state(part).transfer_started);
    assert(!strcmp(xiaotai_coprocessor_host_journal(), "chip\nchip\n"));
    xiaotai_mem_free(part->self);
}

static void transferred(void)
{
    boot_fake();
    image(projects[1]);
    const xiaotai_network_t *part = open(&standard);
    progress_t p = {.part = part};
    assert(part->coprocessor_update(part->self, bytes, sizeof(bytes), progress, &p) == XIAOTAI_OK);
    assert(p.calls == 7 && p.last == XIAOTAI_NETWORK_RESTARTING && state(part).transfer_started);
    assert(!strcmp(xiaotai_coprocessor_host_journal(), full_transfer));
    assert(xiaotai_coprocessor_host()->largest_write == 512 && xiaotai_coprocessor_host()->contiguous);
    xiaotai_coprocessor_host_journal_clear();
    assert(part->coprocessor_update(part->self, bytes, sizeof(bytes), NULL, NULL) == XIAOTAI_ERR_INVALID_STATE);
    assert(state(part).stage == XIAOTAI_NETWORK_POWER_CYCLE && !xiaotai_coprocessor_host_journal()[0]);
    xiaotai_mem_free(part->self);

    boot_fake();
    image(projects[0]);
    xiaotai_coprocessor_config_t custom = standard;
    custom.update_restart_wait_ms = 111;
    custom.update_block_bytes = 768;
    part = open(&custom);
    assert(part->coprocessor_update(part->self, bytes, sizeof(bytes), NULL, NULL) == XIAOTAI_OK);
    assert(!strcmp(xiaotai_coprocessor_host_journal(),
                   "chip\nbegin\nwrite +0 768\nwrite +768 535\nend\nactivate\ndelay 111\n"));
    xiaotai_mem_free(part->self);
}

static void transfer_failure(void)
{
    static const char *const expected[] = {
        "chip\nbegin\n",
        "chip\nbegin\nwrite +0 512\nwrite +512 512\n",
        "chip\nbegin\nwrite +0 512\nwrite +512 512\nwrite +1024 279\nend\n",
        "chip\nbegin\nwrite +0 512\nwrite +512 512\nwrite +1024 279\nend\nactivate\n",
    };
    for (int step = 0; step < 4; ++step) {
        boot_fake();
        image(projects[0]);
        xiaotai_coprocessor_host_t *host = xiaotai_coprocessor_host();
        if (step == 0) host->begin_result = XIAOTAI_ERR_TIMEOUT;
        if (step == 1) { host->fail_write = 2; host->write_error = XIAOTAI_ERR_TIMEOUT; }
        if (step == 2) host->end_result = XIAOTAI_ERR_TIMEOUT;
        if (step == 3) host->activate_result = XIAOTAI_ERR_TIMEOUT;
        const xiaotai_network_t *part = open(&standard);
        progress_t p = {.part = part};
        assert(part->coprocessor_update(part->self, bytes, sizeof(bytes), progress, &p) == XIAOTAI_ERR_TIMEOUT);
        assert(p.last == XIAOTAI_NETWORK_POWER_CYCLE && state(part).error == XIAOTAI_ERR_TIMEOUT &&
               state(part).transfer_started);
        assert(!strcmp(xiaotai_coprocessor_host_journal(), expected[step]));
        assert(part->coprocessor_update(part->self, bytes, sizeof(bytes), NULL, NULL) == XIAOTAI_ERR_INVALID_STATE);
        assert(!strcmp(xiaotai_coprocessor_host_journal(), expected[step]));
        xiaotai_mem_free(part->self);
        if (step == 1) {
            /* Reset both ends, but present exactly the same validated image. */
            boot_fake();
            part = open(&standard);
            assert(!state(part).transfer_started && !xiaotai_coprocessor_host_journal()[0]);
            p = (progress_t){.part = part};
            assert(part->coprocessor_update(part->self, bytes, sizeof(bytes), progress, &p) == XIAOTAI_OK);
            assert(p.last == XIAOTAI_NETWORK_RESTARTING && state(part).transfer_started);
            assert(!strcmp(xiaotai_coprocessor_host_journal(), full_transfer));
            puts("C61 second-block timeout: no operation on same-boot retry; cold boot accepts the same image and activates once");
            xiaotai_mem_free(part->self);
        }
    }
}

static void commands_and_wait(void)
{
    boot_fake();
    const xiaotai_network_t *part = open(&standard);
    char text[6] = "stale";
    assert(part->coprocessor_command(part->self, "info", text, sizeof(text)) == XIAOTAI_ERR_INVALID_ARG);
    assert(!text[0] && !xiaotai_coprocessor_host_journal()[0]);
    assert(part->coprocessor_command(part->self, "INFO", NULL, 1) == XIAOTAI_ERR_INVALID_ARG);
    assert(part->coprocessor_command(part->self, NULL, text, sizeof(text)) == XIAOTAI_ERR_INVALID_ARG);
    xiaotai_coprocessor_host()->command_reply = "C61:OK";
    assert(part->coprocessor_command(part->self, "INFO", text, sizeof(text)) == XIAOTAI_OK);
    assert(!strcmp(text, "C61:O"));
    assert(part->coprocessor_command(part->self, "ROLLBACK", text, sizeof(text)) == XIAOTAI_OK && !text[0]);
    xiaotai_coprocessor_host()->command_result = XIAOTAI_ERR_TIMEOUT;
    assert(part->coprocessor_command(part->self, "ROLLBACK", NULL, 0) == XIAOTAI_ERR_TIMEOUT);
    assert(!strcmp(xiaotai_coprocessor_host_journal(),
                   "command 57545331 INFO 1\ncommand 57545331 ROLLBACK 0\ndelay 4000\ncommand 57545331 ROLLBACK 0\n"));
    xiaotai_mem_free(part->self);

    static const xiaotai_coprocessor_config_t read_only = {.chip_id = 13};
    part = open(&read_only);
    assert(!part->coprocessor_command && !part->coprocessor_update);
    xiaotai_mem_free(part->self);
}

int main(void)
{
    validation();
    transferred();
    transfer_failure();
    commands_and_wait();
    return 0;
}
