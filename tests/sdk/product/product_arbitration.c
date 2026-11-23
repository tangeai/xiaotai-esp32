// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_voice_reliability.py::test_stop_and_diagnostics
#include "product_test.h"

#include "xiaotai/framework.h"
#include "xiaotai/selection.h"
#include "xiaotai_product.h"

#include <stdio.h>
#include <string.h>

enum { CALL, AI, ROOM, STREAM, RECOGNITION, ACTIVITY_COUNT };

#define ACTIVITY(kind) (1U << (kind))
#define FOUR (ACTIVITY(CALL) | ACTIVITY(AI) | ACTIVITY(ROOM) | ACTIVITY(STREAM))
#define FIVE (FOUR | ACTIVITY(RECOGNITION))

static const char *const names[ACTIVITY_COUNT] = {"call", "ai", "room", "stream", "recognition"};

typedef struct {
    unsigned activities;
    /* Rows are the contender, bits are the owners it may revoke. */
    unsigned wins[ACTIVITY_COUNT];
} policy_t;

/* Independent oracle from the legacy workflows; do not derive these rows
 * from the product JSON, generated priorities or framework preemption table. */
static const policy_t szpi_cores3 = {
    .activities = FIVE,
    .wins = {[CALL] = FIVE & ~ACTIVITY(CALL), [AI] = ACTIVITY(ROOM) | ACTIVITY(STREAM)},
};
static const policy_t p4_wt99 = {
    .activities = FOUR,
    .wins = {[CALL] = FOUR & ~ACTIVITY(CALL), [AI] = ACTIVITY(ROOM),
             [ROOM] = ACTIVITY(STREAM), [STREAM] = ACTIVITY(AI)},
};
static const policy_t xmini = {.activities = FOUR};

static const policy_t *product_policy(void)
{
    static const struct {
        const char *product;
        const policy_t *policy;
    } policies[] = {
        {"xiaotai-szpi-s3", &szpi_cores3},
        {"xiaotai-cores3-lite", &szpi_cores3},
        {"xiaotai-jc1060f570c", &szpi_cores3},   /* declares the lckfb S3 priorities */
        {"xiaotai-waveshare-p4-rev3", &p4_wt99},
        {"xiaotai-wt99p461-s1-1v1", &p4_wt99},
        {"xiaotai-xmini-c3-v3", &xmini},
    };
    for (size_t i = 0; i < sizeof(policies) / sizeof(policies[0]); ++i)
        if (!strcmp(XIAOTAI_PRODUCT_NAME, policies[i].product)) return policies[i].policy;
    return NULL;
}

static unsigned service_names(const xiaotai_priority_t *table)
{
    unsigned selected = 0;
    for (const xiaotai_priority_t *entry = table; entry && entry->activity; ++entry)
        for (unsigned i = 0; i < ACTIVITY_COUNT; ++i)
            if (!strcmp(names[i], entry->activity)) selected |= ACTIVITY(i);
    return selected;
}

static unsigned selected_services(void)
{
    unsigned selected = 0;
    for (size_t i = 0; i < xiaotai_selected_module_count; ++i)
        selected |= service_names(xiaotai_selected_modules[i]->activities);
    for (size_t i = 0; i < xiaotai_selected_application_count; ++i)
        selected |= service_names(xiaotai_selected_applications[i]->activities);
    return selected;
}

typedef struct {
    unsigned calls;
    uint32_t id;
    xiaotai_revoke_t reason;
    bool active;
    bool owner;
} revocation_t;

static void revoked(uint32_t id, xiaotai_revoke_t reason, void *ctx)
{
    revocation_t *const record = ctx;
    ++record->calls;
    record->id = id;
    record->reason = reason;
    record->active = xiaotai_activity_active();
    record->owner = xiaotai_on_owner();
}

typedef struct {
    const char *incumbent;
    const char *contender;
    bool preempts;
    bool passed;
} pair_t;

static bool is_current(uint32_t id, const char *name)
{
    const char *current = NULL;
    return xiaotai_activity_current(&current) == id && (id != 0 ? current && !strcmp(current, name) : !current) &&
           xiaotai_activity_active() == (id != 0);
}

static void try_pair(void *ctx)
{
    pair_t *const pair = ctx;
    const uint32_t unchanged = UINT32_MAX;
    uint32_t first_id = 0, next_id = unchanged;
    revocation_t first = {0}, next = {0};
    xiaotai_err_t begin = XIAOTAI_ERR_INVALID_STATE, got = XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_err_t expected = pair->preempts ? XIAOTAI_OK : XIAOTAI_ERR_BUSY;
    const char *issue = "initial owner was not idle";
    /* A zero generation exercises real attachments without opening media
     * or inventing a live network or RTC link. */
    const xiaotai_activity_t holder = {.name = pair->incumbent, .resources = XIAOTAI_RESOURCE_LINK,
                                      .revoked = revoked, .ctx = &first};
    const xiaotai_activity_t request = {.name = pair->contender, .resources = XIAOTAI_RESOURCE_LINK,
                                       .revoked = revoked, .ctx = &next};
    if (!xiaotai_on_owner() || !is_current(0, NULL)) goto done;
    begin = xiaotai_activity_begin(&holder, &first_id);
    issue = "incumbent was not installed";
    if (begin != XIAOTAI_OK || !first_id || !is_current(first_id, pair->incumbent)) goto done;
    got = xiaotai_activity_begin(&request, &next_id);
    issue = "unexpected contender result";
    if (got != expected) goto done;
    issue = "unexpected winner or revocation";
    if (pair->preempts) {
        if (!next_id || next_id == first_id || !is_current(next_id, pair->contender) ||
            first.calls != 1U || first.id != first_id || first.reason != XIAOTAI_REVOKE_PREEMPTED ||
            !first.owner || !first.active || next.calls) goto done;
        xiaotai_activity_end(first_id);
        issue = "a stale end revoked the new owner";
        if (!is_current(next_id, pair->contender)) goto done;
    } else {
        if (next_id != unchanged || first.calls || next.calls || !is_current(first_id, pair->incumbent)) goto done;
    }
    issue = NULL;

done:
    if (got == XIAOTAI_OK) xiaotai_activity_end(next_id);
    if (begin == XIAOTAI_OK) xiaotai_activity_end(first_id);
    if (!issue && (!is_current(0, NULL) || next.calls || first.calls != (unsigned)pair->preempts))
        issue = "explicit end did not return to idle, or invoked revocation";
    pair->passed = !issue;
    if (issue)
        fprintf(stderr, "%s arbitration: %s over %s: %s (hold=%d want=%d got=%d, ids=%u/%u, revocations=%u/%u)\n",
                XIAOTAI_PRODUCT_NAME, pair->contender, pair->incumbent, issue, (int)begin, (int)expected, (int)got,
                (unsigned)first_id, (unsigned)next_id, first.calls, next.calls);
}

int product_arbitration_run(void)
{
    const policy_t *const policy = product_policy();
    const unsigned selected = selected_services();
    if (!policy || selected != policy->activities || xiaotai_on_owner()) {
        fprintf(stderr, "%s arbitration: wrong declaration or test context (mask=%u expected=%u owner=%u)\n",
                XIAOTAI_PRODUCT_NAME, selected, policy ? policy->activities : 0U, (unsigned)xiaotai_on_owner());
        return 1;
    }
    unsigned attempted = 0, failures = 0, takeover = 0;
    for (unsigned holder = 0; holder < ACTIVITY_COUNT; ++holder) {
        if (!(selected & ACTIVITY(holder))) continue;
        for (unsigned contender = 0; contender < ACTIVITY_COUNT; ++contender) {
            if (!(selected & ACTIVITY(contender))) continue;
            pair_t pair = {.incumbent = names[holder], .contender = names[contender],
                           .preempts = (policy->wins[contender] & ACTIVITY(holder)) != 0U};
            product_on_owner(try_pair, &pair);
            ++attempted;
            if (!pair.passed) ++failures;
            else if (pair.preempts) ++takeover;
            printf("%s arbitration: %s over %s -> %s\n", XIAOTAI_PRODUCT_NAME, pair.contender, pair.incumbent,
                   pair.passed ? (pair.preempts ? "PREEMPT" : "BUSY") : "FAIL");
        }
    }
    printf("%s arbitration: %u/%u, %u preemptions\n", XIAOTAI_PRODUCT_NAME, attempted - failures,
           attempted, takeover);
    return failures ? 1 : 0;
}
