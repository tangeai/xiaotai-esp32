// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_service_environment.py
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "internal.h"
#include "platform_internal.h"
#include "product_test.h"
#include "xiaotai/hal.h"
#include "xiaotai/modules/environment.h"
#include "xiaotai/port/kv.h"
#include "xiaotai_kv_host.h"
#include "xiaotai_net_host.h"

typedef struct {
    xiaotai_err_t admission, net, platform, cancel;
    unsigned net_calls, platform_calls;
    uint32_t platform_ticket;
    bool portal_rejected_while_held, cancel_on_accept;
} admission_t;

/* Set and used on the owner only. Wrappers always call the real gates. */
static admission_t *s_capture;
static atomic_bool s_hold_environment_write;
static atomic_uint s_environment_writes;

static const char BASE[] = "http://fixture.invalid/ep";

static const xiaotai_net_submission_t SUBMISSION = {
    .credentials = {.ssid = PRODUCT_TEST_SSID, .password = PRODUCT_TEST_PASSWORD},
    .method = XIAOTAI_PROVISION_PORTAL,
};

xiaotai_err_t __real_xiaotai_net_maintenance_begin(uint32_t *ticket);
xiaotai_err_t __wrap_xiaotai_net_maintenance_begin(uint32_t *ticket)
{
    const xiaotai_err_t result = __real_xiaotai_net_maintenance_begin(ticket);
    if (s_capture) {
        assert(xiaotai_on_owner() && xiaotai_quiesced());
        ++s_capture->net_calls;
        s_capture->net = result;
        assert((result == XIAOTAI_OK) == (*ticket != 0));
    }
    return result;
}

xiaotai_err_t __real_xiaotai_platform_gate_close(uint32_t *ticket);
xiaotai_err_t __wrap_xiaotai_platform_gate_close(uint32_t *ticket)
{
    const xiaotai_err_t result = __real_xiaotai_platform_gate_close(ticket);
    if (s_capture) {
        assert(xiaotai_on_owner() && xiaotai_quiesced() && net_gate_maintenance_held());
        ++s_capture->platform_calls;
        s_capture->platform = result;
        s_capture->platform_ticket = *ticket;
        assert((result == XIAOTAI_OK) == (*ticket != 0));
    }
    return result;
}

xiaotai_err_t __real_xiaotai_kv_commit_async(const char *space, const xiaotai_kv_op_t *ops, size_t count,
                                             xiaotai_kv_done_fn_t done, void *ctx);
xiaotai_err_t __wrap_xiaotai_kv_commit_async(const char *space, const xiaotai_kv_op_t *ops, size_t count,
                                             xiaotai_kv_done_fn_t done, void *ctx)
{
    if (count == 1 && !strcmp(space, PLATFORM_ENVIRONMENT_NAMESPACE) &&
        !strcmp(ops[0].key, PLATFORM_ENVIRONMENT_KEY)) {
        assert(xiaotai_on_owner());
        atomic_fetch_add(&s_environment_writes, 1);
        if (atomic_exchange(&s_hold_environment_write, false)) xiaotai_kv_host_gate(false);
    }
    return __real_xiaotai_kv_commit_async(space, ops, count, done, ctx);
}

static void unexpected_reply(const xiaotai_platform_reply_t *reply, void *ctx)
{
    (void)reply;
    (void)ctx;
    assert(false && "dormant platform delivered a request");
}

static void dormant_no_requests(bool gated)
{
    assert(xiaotai_net_provisioning_boot() && platform.provisioning);
    assert(!platform.objects && !atomic_load(&platform.running) && !platform.session.enabled);
    xiaotai_platform_status_t status;
    xiaotai_platform_status(&status);
    assert(status.phase == XIAOTAI_PLATFORM_STOPPED && status.gated == gated);
    const xiaotai_platform_request_t request = {
        .method = XIAOTAI_PLATFORM_GET, .auth = XIAOTAI_PLATFORM_AUTH_NONE,
        .service = XIAOTAI_PLATFORM_SERVICE_DEVICE, .path = "/fixture",
    };
    assert(xiaotai_platform_request(&request, unexpected_reply, NULL) == XIAOTAI_ERR_PLATFORM_OFFLINE);
    assert(!xiaotai_platform_exchanges_busy());
}

static void attempt(void *ctx)
{
    assert(xiaotai_on_owner() && xiaotai_net_provisioning_boot());
    assert(atomic_load(&platform.initialized) && atomic_load(&platform.started) && platform.provisioning);
    dormant_no_requests(false);
    assert(!xiaotai_quiesced() && !xiaotai_restart_pending());
    admission_t *out = ctx;
    s_capture = out;
    out->admission = xiaotai_environment_set(BASE);
    s_capture = NULL;
    if (out->admission == XIAOTAI_OK) {
        /* Check the opposite direction while this owner job holds both gates. */
        uint32_t duplicate = UINT32_MAX;
        assert(xiaotai_platform_gate_close(&duplicate) == XIAOTAI_ERR_PLATFORM_GATED && duplicate == UINT32_MAX);
        assert(xiaotai_platform_gate_drained(out->platform_ticket));
        dormant_no_requests(true);
        out->portal_rejected_while_held =
            xiaotai_net_provision_submit(&SUBMISSION) == XIAOTAI_ERR_INVALID_STATE;
        if (out->cancel_on_accept) out->cancel = xiaotai_environment_cancel();
    }
}

static void released_gate(void *ctx)
{
    const admission_t *out = ctx;
    dormant_no_requests(false);
    if (out->admission != XIAOTAI_OK) return;
    const uint32_t expired = out->platform_ticket;
    assert(!xiaotai_platform_gate_drained(expired));
    uint32_t new_ticket = 0;
    assert(xiaotai_platform_gate_close(&new_ticket) == XIAOTAI_OK && new_ticket != expired);
    assert(xiaotai_platform_gate_open(expired) == XIAOTAI_ERR_INVALID_ARG);
    assert(!xiaotai_platform_gate_drained(expired) && xiaotai_platform_gate_drained(new_ticket));
    dormant_no_requests(true);
    assert(xiaotai_platform_gate_open(new_ticket) == XIAOTAI_OK);
    dormant_no_requests(false);
}

static void status_is(xiaotai_net_provision_state_t state)
{
    xiaotai_net_provision_status_t current;
    xiaotai_net_provision_status(&current);
    assert(current.exclusive && current.state == state && !current.restart_required);
}

static void wait_released(bool attempted)
{
    for (unsigned i = 0; i < 200; ++i) {
        product_on_owner(NULL, NULL);
        xiaotai_environment_status_t status;
        xiaotai_environment_status(&status);
        if (status.phase == XIAOTAI_ENVIRONMENT_IDLE) {
            assert(status.write_attempted == attempted && !net_gate_maintenance_held());
            assert(!xiaotai_quiesced() && !xiaotai_restart_pending());
            return;
        }
        xiaotai_delay_ms(5);
    }
    assert(false && "environment gates did not release");
}

static void cancel_held(void *ctx)
{
    const admission_t *out = ctx;
    dormant_no_requests(true);
    assert(xiaotai_platform_gate_drained(out->platform_ticket));
    assert(xiaotai_quiesced() && net_gate_maintenance_held() && !xiaotai_restart_pending());
    assert(xiaotai_environment_cancel() == XIAOTAI_OK);
}

static int write_timeout(void)
{
    admission_t out = {0};
    atomic_store(&s_hold_environment_write, true);
    product_on_owner(attempt, &out);
    assert(out.admission == XIAOTAI_OK && out.portal_rejected_while_held);
    assert(xiaotai_kv_host_wait_held(3000) && !atomic_load(&s_hold_environment_write));

    xiaotai_environment_status_t status;
    const int64_t deadline = xiaotai_time_us() + 15000000;
    do {
        xiaotai_delay_ms(10);
        xiaotai_environment_status(&status);
    } while (status.phase != XIAOTAI_ENVIRONMENT_ERROR_HELD && xiaotai_time_us() < deadline);
    assert(status.phase == XIAOTAI_ENVIRONMENT_ERROR_HELD && status.write_attempted);
    assert(status.error == XIAOTAI_ERR_TIMEOUT && status.environment.unconfirmed && !status.environment.saved[0]);
    assert(xiaotai_quiesced() && net_gate_maintenance_held() && !xiaotai_restart_pending());
    assert(xiaotai_net_provision_submit(&SUBMISSION) == XIAOTAI_ERR_INVALID_STATE);

    /* A timed-out write can still land. It releases no gate and cannot start
     * networking; release is retried by the owner only after storage drains. */
    xiaotai_kv_host_gate(true);
    assert(xiaotai_kv_flush(3000) == XIAOTAI_OK);
    assert(net_gate_maintenance_held() && atomic_load(&s_environment_writes) == 1);
    product_on_owner(cancel_held, &out);
    wait_released(true);
    product_on_owner(released_gate, &out);
    char stored[XIAOTAI_PLATFORM_BASE_MAX + 1U];
    assert(xiaotai_kv_get_str_strict(PLATFORM_ENVIRONMENT_NAMESPACE, PLATFORM_ENVIRONMENT_KEY,
                                    stored, sizeof(stored), 1000) == XIAOTAI_OK);
    assert(strcmp(stored, BASE) == 0 && atomic_load(&s_environment_writes) == 1);
    status_is(XIAOTAI_NET_PROVISION_OPEN);
    return 0;
}

static void check_bad_address(void *ctx)
{
    const bool verifying = *(const bool *)ctx;
    const bool held = xiaotai_net_provisioning_held();
    assert(held == verifying);
    assert(xiaotai_on_owner() && !xiaotai_quiesced() && !net_gate_maintenance_held());
    xiaotai_environment_status_t before, after;
    xiaotai_environment_status(&before);
    admission_t captured = {0};
    s_capture = &captured;
    const xiaotai_err_t result = xiaotai_environment_set("ftp://bad.test");
    s_capture = NULL;
    printf("environment invalid base: verifying=%d result=%d, no gate or storage write\n", verifying, result);
    assert(result == (verifying ? XIAOTAI_ERR_INVALID_STATE : XIAOTAI_ERR_INVALID_ARG));
    assert(!captured.net_calls && !captured.platform_calls && !atomic_load(&s_environment_writes));
    xiaotai_environment_status(&after);
    assert(before.revision == after.revision && !xiaotai_quiesced() && held == xiaotai_net_provisioning_held());
}

int product_environment_admission(bool verifying, bool timeout)
{
    assert(xiaotai_net_host_wait_calls(XIAOTAI_NET_HOST_AP_START, 1, 3000));
    assert(xiaotai_net_host_sync(3000) == XIAOTAI_OK);
    status_is(XIAOTAI_NET_PROVISION_OPEN);
    if (timeout) return write_timeout();
    if (verifying) {
        const xiaotai_net_host_step_t hold_join = {.result = XIAOTAI_OK, .hold = true};
        assert(xiaotai_net_host_step(XIAOTAI_NET_HOST_JOIN, &hold_join) == XIAOTAI_OK);
        assert(xiaotai_net_provision_submit(&SUBMISSION) == XIAOTAI_OK);
        assert(xiaotai_net_host_wait_held(3000));
        status_is(XIAOTAI_NET_PROVISION_VERIFYING);
    }

    product_on_owner(check_bad_address, &verifying);
    admission_t out = {.cancel_on_accept = true};
    product_on_owner(attempt, &out);
    printf("environment during %s: admission=%d net(%u)=%d platform(%u)=%d\n",
           verifying ? "credential verification" : "idle provisioning",
           out.admission, out.net_calls, out.net, out.platform_calls, out.platform);
    if (verifying) {
        status_is(XIAOTAI_NET_PROVISION_VERIFYING);
        xiaotai_net_host_release();
    }
    wait_released(false);
    product_on_owner(released_gate, &out);
    xiaotai_platform_environment_t environment;
    xiaotai_platform_environment(&environment);
    assert(!environment.saved[0] && !environment.unconfirmed);
    char stored[XIAOTAI_PLATFORM_BASE_MAX + 1U];
    assert(xiaotai_kv_get_str(PLATFORM_ENVIRONMENT_NAMESPACE, PLATFORM_ENVIRONMENT_KEY,
                             stored, sizeof(stored), 1000) == XIAOTAI_ERR_NOT_FOUND);

    if (verifying) {
        assert(out.admission == XIAOTAI_ERR_INVALID_STATE && out.net_calls == 1);
        assert(out.net == XIAOTAI_ERR_INVALID_STATE && out.platform_calls == 0);
        return 0;
    }
    status_is(XIAOTAI_NET_PROVISION_OPEN);
    assert(out.net_calls == 1 && out.net == XIAOTAI_OK && out.platform_calls == 1);
    if (out.admission != XIAOTAI_OK) {
        assert(out.platform == out.admission);
        fprintf(stderr, "idle provisioning admitted framework and network leases but failed at platform gate\n");
        return 1;
    }
    assert(out.platform == XIAOTAI_OK && out.cancel == XIAOTAI_OK && out.portal_rejected_while_held);
    return 0;
}
