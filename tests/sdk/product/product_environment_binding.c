// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_environment_control.py
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "internal.h"
#include "product_platform.h"
#include "product_test.h"
#include "xiaotai/modules/environment.h"
#include "xiaotai/platform.h"
#include "xiaotai_net_host.h"
#ifndef PRODUCT_TEST_ENVIRONMENT_REQUIRES_BINDING_IDLE
#define PRODUCT_TEST_ENVIRONMENT_REQUIRES_BINDING_IDLE 0
#endif
#if PRODUCT_TEST_CLOCK_GATE
#include "recognition_journal.h"
#include "xiaotai/modules/recognition.h"
#include "xiaotai/port/kv.h"

static const char *s_journal;
static xiaotai_err_t s_journal_error;

void product_environment_journal_prepare(const char *mode)
{
    if (!strcmp(mode, "pending")) {
        s_journal = "{\"version\":1,\"device_id\":\"" PRODUCT_FIXTURE_DEVICE
                    "\",\"request_id\":\"journal-fixture\",\"revision\":1,\"seconds\":1}";
        s_journal_error = XIAOTAI_ERR_INVALID_STATE;
    } else if (!strcmp(mode, "corrupt")) {
        s_journal = "{not-json";
        s_journal_error = XIAOTAI_RECOGNITION_JOURNAL_INVALID_RESPONSE;
    } else assert(!strcmp(mode, "absent"));
    if (s_journal) assert(xiaotai_kv_set_str("xt_recognition", "pending", s_journal, 3000) == XIAOTAI_OK);
}

static void journal_settled(void *ctx)
{
    const xiaotai_recognition_journal_state_t state = xiaotai_recognition_journal_mirror()->state;
    *(bool *)ctx = (state == XIAOTAI_RECOGNITION_JOURNAL_ABSENT || state == XIAOTAI_RECOGNITION_JOURNAL_PRESENT ||
                    state == XIAOTAI_RECOGNITION_JOURNAL_ERROR) && !xiaotai_recognition_journal_busy();
}

static void check_offline_journal(void *unused)
{
    (void)unused;
    xiaotai_net_status_t net;
    xiaotai_net_status(&net);
    assert(net.state != XIAOTAI_NET_UP && !xiaotai_quiesced() && !xiaotai_activity_current(NULL));
    const xiaotai_err_t admission = xiaotai_environment_set("http://fixture.invalid/environment");
    xiaotai_environment_status_t environment;
    xiaotai_environment_status(&environment);
    const xiaotai_recognition_journal_state_t journal = xiaotai_recognition_journal_mirror()->state;
    assert(journal == (s_journal_error == XIAOTAI_OK ? XIAOTAI_RECOGNITION_JOURNAL_ABSENT :
        s_journal_error == XIAOTAI_ERR_INVALID_STATE ? XIAOTAI_RECOGNITION_JOURNAL_PRESENT :
        XIAOTAI_RECOGNITION_JOURNAL_ERROR));
    assert(admission == s_journal_error && environment.error == s_journal_error);
    assert(!environment.write_attempted && !xiaotai_restart_pending());
    if (!s_journal) {
        assert(environment.phase == XIAOTAI_ENVIRONMENT_WAITING && xiaotai_quiesced());
        assert(xiaotai_environment_cancel() == XIAOTAI_OK);
    } else {
        assert(environment.phase == XIAOTAI_ENVIRONMENT_IDLE && !xiaotai_quiesced() && !net_gate_maintenance_held());
    }
    printf("offline journal: state=%u admission=0x%x wrote=0\n", (unsigned)journal, (unsigned)admission);
}

void product_environment_journal(void)
{
    assert(xiaotai_net_host_wait_calls(XIAOTAI_NET_HOST_JOIN, 1, 3000));
    assert(xiaotai_net_host_sync(3000) == XIAOTAI_OK);
    bool ready = false;
    for (unsigned tries = 0; tries < 400 && !ready; ++tries) {
        product_on_owner(journal_settled, &ready);
        if (!ready) xiaotai_delay_ms(5);
    }
    assert(ready);
    product_on_owner(check_offline_journal, NULL);
    char actual[128];
    const xiaotai_err_t read = xiaotai_kv_get_str("xt_recognition", "pending", actual, sizeof(actual), 3000);
    if (!s_journal) assert(read == XIAOTAI_ERR_NOT_FOUND);
    else assert(read == XIAOTAI_OK && !strcmp(actual, s_journal));
}
#endif

typedef enum { ONLINE, VERIFY, CONFIRMED } binding_case_t;

static void check_admission(void *ctx)
{
    const binding_case_t scenario = *(const binding_case_t *)ctx;
    assert(xiaotai_on_owner() && !xiaotai_activity_current(NULL) && !xiaotai_quiesced());
    assert(!net_gate_maintenance_held() && !xiaotai_restart_pending());
    xiaotai_platform_status_t before, after;
    xiaotai_platform_status(&before);
    assert(before.phase == XIAOTAI_PLATFORM_ONLINE && before.signaling && before.discovered && !before.gated);

    /* Keep reports and admission in one owner job. The platform cannot
     * finish the binding fence while this job is running. */
    if (scenario != ONLINE) xiaotai_platform_report_unbound(scenario == CONFIRMED);
    xiaotai_platform_status(&after);
    assert(after.verifying == (scenario == VERIFY));
    if (scenario != ONLINE)
        assert(after.generation != before.generation && !after.authorized && !after.signaling);

    xiaotai_err_t expected = XIAOTAI_OK;
    if (PRODUCT_TEST_ENVIRONMENT_REQUIRES_BINDING_IDLE && scenario != ONLINE) {
        uint32_t ticket = UINT32_MAX;
        assert(xiaotai_platform_gate_close_binding_idle(&ticket) == XIAOTAI_ERR_BUSY && ticket == UINT32_MAX);
        expected = XIAOTAI_ERR_BUSY;
#if PRODUCT_TEST_CLOCK_GATE
        /* The report can invalidate the recognition journal's identity scope.
         * The earlier owner guard retains its result if it sees that first. */
        const xiaotai_err_t guard = xiaotai_recognition_maintenance_check();
        assert(guard == XIAOTAI_OK || guard == XIAOTAI_ERR_INVALID_STATE);
        if (guard != XIAOTAI_OK) expected = guard;
#endif
    }
    const xiaotai_err_t admission = xiaotai_environment_set("http://fixture.invalid/environment");
    xiaotai_environment_status_t environment;
    xiaotai_environment_status(&environment);
    xiaotai_platform_status(&after);
    printf("binding=%u verifying=%u generation=%u->%u admission=0x%x env_phase=%u write=%u\n",
           (unsigned)scenario, (unsigned)after.verifying, before.generation, after.generation,
           (unsigned)admission, (unsigned)environment.phase, (unsigned)environment.write_attempted);
    assert(!environment.write_attempted && !xiaotai_restart_pending());
    if (expected != XIAOTAI_OK) {
        assert(admission == expected && environment.error == expected);
        assert(environment.phase == XIAOTAI_ENVIRONMENT_IDLE);
        assert(!xiaotai_quiesced() && !net_gate_maintenance_held() && !after.gated);
    } else {
        assert(admission == XIAOTAI_OK && environment.phase == XIAOTAI_ENVIRONMENT_WAITING);
        assert(xiaotai_quiesced() && net_gate_maintenance_held() && after.gated);
        assert(xiaotai_environment_cancel() == XIAOTAI_OK);
    }
}

typedef struct {
    binding_case_t scenario;
    bool ran;
} admission_test_t;

static void check_when_ready(void *ctx)
{
    admission_test_t *test = ctx;
#if PRODUCT_TEST_CLOCK_GATE
    /* A queued recognition refresh can run between owner jobs. Probe the
     * precondition in the same turn that admits (or refuses) the switch. */
    if (xiaotai_recognition_maintenance_check() != XIAOTAI_OK) return;
#endif
    check_admission(&test->scenario);
    test->ran = true;
}

void product_environment_binding(const char *name)
{
    binding_case_t scenario = ONLINE;
    if (!strcmp(name, "verify")) scenario = VERIFY;
    else if (!strcmp(name, "confirmed") || !strcmp(name, "rejected")) scenario = CONFIRMED;
    else assert(!strcmp(name, "online"));
    product_platform_online();
    /* An online snapshot can precede the network worker's history write.
     * Drain that lease so the environment request reaches the binding gate. */
    assert(xiaotai_net_host_sync(5000) == XIAOTAI_OK);
    if (strcmp(name, "rejected")) {
        admission_test_t test = {.scenario = scenario};
        for (unsigned tries = 0; tries != 200 && !test.ran; ++tries) {
            product_on_owner(check_when_ready, &test);
            if (!test.ran) xiaotai_delay_ms(5);
        }
        assert(test.ran);
        return;
    }

    xiaotai_platform_report_unbound(true);
    xiaotai_platform_status_t status;
    for (unsigned tries = 0; tries != 200; ++tries) {
        xiaotai_platform_status(&status);
        if (status.phase == XIAOTAI_PLATFORM_UNBOUND) break;
        xiaotai_delay_ms(5);
    }
    assert(status.phase == XIAOTAI_PLATFORM_UNBOUND && !status.verifying && !status.gated);
    /* Test the narrow gate even if the owner has started the pairing UX. */
    uint32_t ticket = UINT32_MAX;
    assert(xiaotai_platform_gate_close_binding_idle(&ticket) == XIAOTAI_ERR_BUSY && ticket == UINT32_MAX);
    assert(xiaotai_platform_gate_close(&ticket) == XIAOTAI_OK && ticket != UINT32_MAX);
    assert(xiaotai_platform_gate_open(ticket) == XIAOTAI_OK);
}
