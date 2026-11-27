// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "xiaotai/framework.h"
#include "xiaotai/hal.h"
#include "xiaotai/net.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai_coprocessor_host.h"
#include "xiaotai_net_host.h"

#define WAIT_MS 3000U
#define FIRST_VERSION "3.0.7-first-fixture"
#define RETRY_VERSION "3.0.7-xiaotai.4"

static xiaotai_signal_t paused, resume;
static xiaotai_coprocessor_host_t *peer;
static unsigned marker_reads;

bool __wrap_xiaotai_mem_has_external(void) { return true; }

static void pause_on_worker(void)
{
    assert(!xiaotai_on_owner());
    xiaotai_signal_give(&paused);
    assert(xiaotai_signal_take(&resume, WAIT_MS));
}

xiaotai_err_t __real_xiaotai_kv_get_u8(const char *space, const char *key, uint8_t *value, uint32_t timeout);
xiaotai_err_t __wrap_xiaotai_kv_get_u8(const char *space, const char *key, uint8_t *value, uint32_t timeout)
{
    if (strcmp(space, "c6_update") || strcmp(key, "attempt"))
        return __real_xiaotai_kv_get_u8(space, key, value, timeout);
    assert(++marker_reads == peer->apps && peer->apps == peer->versions && peer->apps <= 2);
    /* The updater has read the APP descriptor and has not returned from prepare(). */
    pause_on_worker();
    if (marker_reads == 1) return XIAOTAI_ERR_TIMEOUT;
    return __real_xiaotai_kv_get_u8(space, key, value, timeout);
}

xiaotai_err_t __real_xiaotai_coprocessor_link_wait(uint32_t timeout);
xiaotai_err_t __wrap_xiaotai_coprocessor_link_wait(uint32_t timeout)
{
    assert(timeout == 15000);
    if (peer->waits == 1) pause_on_worker(); /* Before the retry's descriptor read. */
    return __real_xiaotai_coprocessor_link_wait(timeout);
}

xiaotai_err_t __real_xiaotai_coprocessor_link_ota_begin(void);
xiaotai_err_t __wrap_xiaotai_coprocessor_link_ota_begin(void)
{
    uint8_t durable = 0;
    assert(__real_xiaotai_kv_get_u8("c6_update", "attempt", &durable, WAIT_MS) == XIAOTAI_OK && durable == 1);
    assert(!peer->begins && !peer->writes);
    /* No WRITING notification has fired. BEGIN may itself fail. */
    pause_on_worker();
    return __real_xiaotai_coprocessor_link_ota_begin();
}

static xiaotai_net_status_t status(void)
{
    xiaotai_net_status_t now;
    xiaotai_net_status(&now);
    return now;
}

static bool snapshot(const char *phase, const char *version, unsigned calls)
{
    xiaotai_coprocessor_info_t info = {0};
    assert(peer->versions == calls && peer->apps == calls && !peer->commands);
    const xiaotai_err_t err = xiaotai_net_coprocessor_info(false, &info);
    const bool correct = version ?
        err == XIAOTAI_OK && !strcmp(info.project, "eh_cp_wifi_sta") && !strcmp(info.version, version) &&
        !strcmp(info.platform, "v5.5.4") && !strcmp(info.protocol, "3.0.7") && strlen(info.elf_sha256) == 64 :
        err == XIAOTAI_ERR_NOT_FOUND && !info.version[0];
    fprintf(stderr, "%s cached=%d version=%s Hosted-apps=%u Hosted-versions=%u %s\n",
            phase, (int)err, info.version, peer->apps, peer->versions, correct ? "PASS" : "FAIL");
    assert(peer->versions == calls && peer->apps == calls && !peer->commands);
    return correct;
}

static void select_update_source(void)
{
    static const uint8_t source_digest[32] = {
        0x1e, 0xca, 0x43, 0xdb, 0x04, 0xc7, 0x35, 0xcc, 0x50, 0x5f, 0xe9, 0xd0, 0x5e, 0xbb, 0x73, 0xcf,
        0x62, 0x11, 0x56, 0xc1, 0xdf, 0x83, 0x1c, 0x44, 0x4b, 0xa2, 0x8a, 0xe9, 0x04, 0xc3, 0xab, 0x03,
    };
    strcpy(peer->version, RETRY_VERSION);
    memcpy(peer->elf_sha256, source_digest, sizeof(source_digest));
}

int main(void)
{
    assert(xiaotai_signal_init(&paused) == XIAOTAI_OK && xiaotai_signal_init(&resume) == XIAOTAI_OK);
    assert(xiaotai_kv_init() == XIAOTAI_OK);
    assert(xiaotai_hal_init() == XIAOTAI_OK);
    const xiaotai_network_t *part = xiaotai_hal_network(0);
    assert(part && part->prepare && part->coprocessor_info && !part->coprocessor_command && !part->coprocessor_update);
    xiaotai_coprocessor_host_reset();
    peer = xiaotai_coprocessor_host();
    peer->chip = 0x0d;
    peer->major = 3;
    peer->patch = 7;
    peer->app_magic = 0xabcd5432U;
    peer->elf_sha256[0] = 0xd1;
    strcpy(peer->project, "eh_cp_wifi_sta");
    strcpy(peer->version, FIRST_VERSION);
    strcpy(peer->platform, "v5.5.4");
    peer->begin_result = XIAOTAI_ERR_TIMEOUT;
    static const xiaotai_module_t net = {.name = "net", .init = xiaotai_engine_net_init,
                                        .start = xiaotai_engine_net_start};
    const xiaotai_module_t *const modules[] = {&net};
    assert(xiaotai_framework_start(&(xiaotai_framework_config_t){.modules = modules, .module_count = 1}) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&paused, WAIT_MS));
    assert(status().state == XIAOTAI_NET_PREPARING && status().prepare.stage == XIAOTAI_NET_PREPARE_VERIFYING);
    bool correct = snapshot("initial descriptor before prepare return", FIRST_VERSION, 1);
    xiaotai_signal_give(&resume);
    assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK);
    assert(status().state == XIAOTAI_NET_BLOCKED && status().prepare.fault == XIAOTAI_NET_FAULT_NO_LINK);
    correct = snapshot("initial descriptor after failed marker read", FIRST_VERSION, 1) && correct;

    assert(xiaotai_net_retry() == XIAOTAI_OK && xiaotai_signal_take(&paused, WAIT_MS));
    assert(status().state == XIAOTAI_NET_PREPARING && status().prepare.stage == XIAOTAI_NET_PREPARE_CHECKING);
    correct = snapshot("retry before descriptor read clears old value", NULL, 1) && correct;
    select_update_source();
    xiaotai_signal_give(&resume);
    assert(xiaotai_signal_take(&paused, WAIT_MS));
    correct = snapshot("retry descriptor before prepare return replaces old value", RETRY_VERSION, 2) && correct;
    xiaotai_signal_give(&resume);

    assert(xiaotai_signal_take(&paused, WAIT_MS));
    assert(status().prepare.stage == XIAOTAI_NET_PREPARE_VERIFYING);
    correct = snapshot("durable marker before BEGIN clears identity", NULL, 2) && correct;
    xiaotai_signal_give(&resume);
    assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK);
    assert(status().state == XIAOTAI_NET_BLOCKED && status().prepare.fault == XIAOTAI_NET_FAULT_POWER_CYCLE);
    correct = snapshot("failed BEGIN does not restore identity", NULL, 2) && correct;
    assert(peer->begins == 1 && !peer->writes && !peer->starts && peer->waits == 2);
    xiaotai_net_reset();
    xiaotai_framework_reset();
    xiaotai_signal_deinit(&resume);
    xiaotai_signal_deinit(&paused);
    return correct ? 0 : 1;
}
