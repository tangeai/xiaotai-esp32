// SPDX-License-Identifier: MIT
/* mod_provisioning_intent: the four intents.
 *   provisioning.begin  PREEMPT only; FORGET (or any other bit) refused with
 *                       INVALID_ARG and reported as a failed begin; may come
 *                       over the console (AT+INTENT)
 *   provisioning.forget the factory gesture: begin with FORGET; local only, so
 *                       the console's AT+INTENT gets NOT_ALLOWED and nothing
 *                       happens
 *   provisioning.cancel give up: the engine's close once the begin is handed
 *   provisioning.retry  "try again": storing again in STORE_FAILED, the link
 *                       again while offline (BLOCKED, WAITING, HELD), refused
 *                       otherwise
 * plus scan and network list forwarding and the restart hold forwarding. */
#include <string.h>
#include "harness.h"

static xiaotai_err_t post(const char *name, int32_t value, bool external)
{
    const xiaotai_args_t args = {.value = value};
    const xiaotai_err_t err = external ? xiaotai_intent_post_external(name, &args) : xiaotai_intent_post(name, &args);
    drain();
    return err;
}

static void finish(void *ctx) { fake_net_finish(*(xiaotai_err_t *)ctx); }

static void store_failed(void *ctx)
{
    (void)ctx;
    fake_net_provision(XIAOTAI_NET_PROVISION_STORE_FAILED, XIAOTAI_FAIL);
}

static void link_state(void *ctx)
{
    fake_net.link.state = *(xiaotai_net_state_t *)ctx;
    fake_net_link_changed();
}

int main(void)
{
    xiaotai_provisioning_stats_t stats;
    fake_net_reset();
    CHECK(harness_start(20) == 0);
    CHECK(xiaotai_intent_exists(XIAOTAI_INTENT_PROVISIONING_BEGIN) &&
          xiaotai_intent_exists(XIAOTAI_INTENT_PROVISIONING_FORGET) &&
          xiaotai_intent_exists(XIAOTAI_INTENT_PROVISIONING_CANCEL) &&
          xiaotai_intent_exists(XIAOTAI_INTENT_PROVISIONING_RETRY));

    /* provisioning.begin with FORGET: refused, reported, nothing erased. */
    CHECK(post(XIAOTAI_INTENT_PROVISIONING_BEGIN, XIAOTAI_PROVISIONING_FORGET, false) == XIAOTAI_OK);
    CHECK(strstr(trace, "failed:begin:258 ") != NULL && fake_net.reprovisions == 0 && !owner_quiesced());
    CHECK(post(XIAOTAI_INTENT_PROVISIONING_BEGIN, XIAOTAI_PROVISIONING_FORGET | XIAOTAI_PROVISIONING_PREEMPT,
               true) == XIAOTAI_OK);
    CHECK(trace_count("failed:begin:258 ") == 2 && fake_net.reprovisions == 0);
    xiaotai_provisioning_stats(&stats);
    CHECK(stats.begins_refused == 2);

    /* The console may not post the factory gesture: NOT_ALLOWED, nothing queued. */
    trace_clear();
    CHECK(post(XIAOTAI_INTENT_PROVISIONING_FORGET, 0, true) == XIAOTAI_ERR_NOT_ALLOWED);
    CHECK(fake_net.reprovisions == 0 && trace[0] == '\0' && !owner_quiesced());

    /* provisioning.begin from the console with PREEMPT: an activity is ended. */
    CHECK(owner_activity_begin() != 0);
    CHECK(post(XIAOTAI_INTENT_PROVISIONING_BEGIN, 0, true) == XIAOTAI_OK);
    CHECK(strstr(trace, "failed:begin:28673 ") != NULL && fake_net.reprovisions == 0);   /* XIAOTAI_ERR_BUSY */
    CHECK(post(XIAOTAI_INTENT_PROVISIONING_BEGIN, XIAOTAI_PROVISIONING_PREEMPT, true) == XIAOTAI_OK);
    CHECK(wait_reprovisions(1, 1000));
    CHECK(!fake_net.last_forget && atomic_load(&revocations) == 1);
    xiaotai_err_t ok = XIAOTAI_OK;
    on_owner(finish, &ok);
    CHECK(phase() == XIAOTAI_PROVISIONING_WAITING);

    /* provisioning.cancel: the begin was handed, so the engine closes. */
    CHECK(post(XIAOTAI_INTENT_PROVISIONING_CANCEL, 0, false) == XIAOTAI_OK);
    CHECK(fake_net.closes == 1);
    xiaotai_provisioning_stats(&stats);
    CHECK(stats.cancels == 1);
    fake_net.provision.state = XIAOTAI_NET_PROVISION_CLOSED;
    xiaotai_net_state_t up = XIAOTAI_NET_UP;
    on_owner(link_state, &up);

    /* provisioning.forget from a local input: erases with the request. */
    CHECK(owner_activity_begin() != 0);
    CHECK(post(XIAOTAI_INTENT_PROVISIONING_FORGET, XIAOTAI_PROVISIONING_PREEMPT, false) == XIAOTAI_OK);
    CHECK(wait_reprovisions(2, 1000));
    CHECK(fake_net.last_forget && atomic_load(&revocations) == 2);
    on_owner(finish, &ok);

    /* provisioning.retry: storing again after a store failure (PV10)... */
    on_owner(store_failed, NULL);
    CHECK(phase() == XIAOTAI_PROVISIONING_STORE_FAILED);
    CHECK(strstr(trace, "failed:store:-1 ") != NULL);
    CHECK(post(XIAOTAI_INTENT_PROVISIONING_RETRY, 0, false) == XIAOTAI_OK);
    CHECK(fake_net.store_retries == 1 && fake_net.retries == 0);
    /* ...no automatic store retry by default (explicit only). */
    xiaotai_delay_ms(300);
    drain();
    CHECK(fake_net.store_retries == 1);
    /* ...the link again while offline... */
    fake_net.provision.state = XIAOTAI_NET_PROVISION_OPEN;
    xiaotai_net_state_t waiting = XIAOTAI_NET_WAITING;
    on_owner(link_state, &waiting);
    CHECK(owner_retry() == XIAOTAI_OK && fake_net.retries == 1);
    xiaotai_net_state_t held = XIAOTAI_NET_HELD;
    on_owner(link_state, &held);
    CHECK(owner_retry() == XIAOTAI_OK && fake_net.retries == 2);
    /* ...and nothing while the link is up and nothing failed. */
    on_owner(link_state, &up);
    CHECK(owner_retry() == XIAOTAI_ERR_INVALID_STATE && fake_net.retries == 2 && fake_net.store_retries == 1);
    /* The engine's refusal is the result and the status error. */
    on_owner(link_state, &waiting);
    fake_net.retry_result = XIAOTAI_ERR_TIMEOUT;
    CHECK(owner_retry() == XIAOTAI_ERR_TIMEOUT);
    xiaotai_provisioning_status_t status;
    xiaotai_provisioning_status(&status);
    CHECK(status.error == XIAOTAI_ERR_TIMEOUT);
    xiaotai_provisioning_stats(&stats);
    CHECK(stats.retries == 3);

    /* Scan and list forward to the engine; no password field exists. */
    CHECK(xiaotai_provisioning_scan() == XIAOTAI_OK && fake_net.scans == 1);
    xiaotai_net_network_t networks[1];
    xiaotai_net_scan_status_t scan;
    CHECK(xiaotai_provisioning_networks(networks, 1, &scan) == 2);
    CHECK(strcmp(networks[0].ssid, "home") == 0 && networks[0].saved && scan.state == XIAOTAI_NET_SCAN_READY);

    /* Application holds are the framework's holds, counted. */
    const int before = framework_holds();
    xiaotai_provisioning_restart_hold();
    CHECK(framework_holds() == before + 1);
    xiaotai_provisioning_restart_release();
    CHECK(framework_holds() == before);
    xiaotai_provisioning_stats(&stats);
    CHECK(stats.restart_holds == 1);
    harness_stop();
    puts("mod_provisioning_intent: passed");
    return 0;
}
