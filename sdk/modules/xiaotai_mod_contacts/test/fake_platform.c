// SPDX-License-Identifier: MIT
/* Fake platform core (see fake_platform.h). Replies and signals run on the
 * framework owner task, as the real platform core delivers them. */
#include "fake_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xiaotai/framework.h"
#include "xiaotai/identity.h"
#include "xiaotai/port/os.h"

#define FAKE_REQUESTS 128U
#define FAKE_SUBSCRIPTIONS 8U
#define FAKE_BODY_MAX 65536U

static fake_request_t s_requests[FAKE_REQUESTS];
static size_t s_request_count;
static xiaotai_err_t s_refuse;
static xiaotai_platform_requirement_t s_requirement;
static size_t s_requirements;
static char s_services[4][XIAOTAI_PLATFORM_SERVICE_NAME_MAX];
static size_t s_service_count;
static struct {
    const char *type;
    xiaotai_platform_signal_fn_t handler;
    void *ctx;
} s_subscriptions[FAKE_SUBSCRIPTIONS];
static size_t s_subscription_count;

static void fail(const char *what)
{
    fprintf(stderr, "fake platform: %s\n", what);
    exit(1);
}

void fake_platform_reset(void)
{
    memset(s_requests, 0, sizeof(s_requests));
    s_request_count = 0;
    s_refuse = XIAOTAI_OK;
    memset(&s_requirement, 0, sizeof(s_requirement));
    s_requirements = 0;
    memset(s_services, 0, sizeof(s_services));
    s_service_count = 0;
    memset(s_subscriptions, 0, sizeof(s_subscriptions));
    s_subscription_count = 0;
}

/* ---------------------------------------------------- the platform's API */

xiaotai_err_t xiaotai_platform_require(const xiaotai_platform_requirement_t *requirement)
{
    if (!requirement) return XIAOTAI_ERR_INVALID_ARG;
    s_requirement = *requirement;
    ++s_requirements;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_platform_require_service(const char *service)
{
    if (!service || s_service_count == 4U) return XIAOTAI_ERR_INVALID_ARG;
    snprintf(s_services[s_service_count++], sizeof(s_services[0]), "%s", service);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_platform_signal_subscribe(const char *type, const char *channel,
                                                xiaotai_platform_signal_fn_t handler, void *ctx)
{
    (void)channel;
    if (!type || !handler || s_subscription_count == FAKE_SUBSCRIPTIONS) return XIAOTAI_ERR_INVALID_ARG;
    s_subscriptions[s_subscription_count].type = type;
    s_subscriptions[s_subscription_count].handler = handler;
    s_subscriptions[s_subscription_count].ctx = ctx;
    ++s_subscription_count;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_platform_request(const xiaotai_platform_request_t *request, xiaotai_platform_reply_fn_t reply,
                                       void *ctx)
{
    if (!request || !reply || !request->service || !request->path) return XIAOTAI_ERR_INVALID_ARG;
    if (request->method == XIAOTAI_PLATFORM_GET && request->body) return XIAOTAI_ERR_INVALID_ARG;
    if (s_refuse != XIAOTAI_OK) {
        xiaotai_err_t err = s_refuse;
        s_refuse = XIAOTAI_OK;
        return err;
    }
    if (s_request_count == FAKE_REQUESTS) fail("request table full");
    fake_request_t *r = &s_requests[s_request_count++];
    snprintf(r->service, sizeof(r->service), "%s", request->service);
    snprintf(r->path, sizeof(r->path), "%s", request->path);
    r->method = request->method;
    r->auth = request->auth;
    r->flags = request->flags;
    r->timeout_ms = request->timeout_ms;
    r->has_body = request->body != NULL;
    if (request->body) snprintf(r->body, sizeof(r->body), "%s", request->body);
    r->generation = xiaotai_identity_generation();
    r->reply = reply;
    r->ctx = ctx;
    return XIAOTAI_OK;
}

/* --------------------------------------------------------- test controls */

size_t fake_platform_request_count(void) { return s_request_count; }

const fake_request_t *fake_platform_request(size_t index)
{
    if (index >= s_request_count) fail("no such request");
    return &s_requests[index];
}

void fake_platform_refuse_next(xiaotai_err_t err) { s_refuse = err; }

xiaotai_platform_requirement_t fake_platform_requirement(void) { return s_requirement; }
size_t fake_platform_requirements(void) { return s_requirements; }
size_t fake_platform_service_count(void) { return s_service_count; }
const char *fake_platform_service(size_t index) { return index < s_service_count ? s_services[index] : NULL; }
size_t fake_platform_subscription_count(void) { return s_subscription_count; }
const char *fake_platform_subscription(size_t index)
{
    return index < s_subscription_count ? s_subscriptions[index].type : NULL;
}

typedef struct {
    size_t index;
    xiaotai_platform_reply_t reply;
    const char *type;
    xiaotai_signal_t done;
} job_t;

static void run_and_wait(void (*fn)(void *), job_t *job)
{
    if (xiaotai_signal_init(&job->done) != XIAOTAI_OK) fail("signal");
    if (xiaotai_run(fn, job) != XIAOTAI_OK) fail("owner queue");
    if (!xiaotai_signal_take(&job->done, 5000)) fail("owner did not run the job");
    xiaotai_signal_deinit(&job->done);
}

static void answer_job(void *ctx)
{
    job_t *job = ctx;
    fake_request_t *r = &s_requests[job->index];
    r->answered = true;
    r->reply(&job->reply, r->ctx);
    xiaotai_signal_give(&job->done);
}

void fake_platform_answer_generation(size_t index, xiaotai_err_t result, int http_status, const char *body,
                                     uint32_t generation)
{
    if (index >= s_request_count) fail("answer to no request");
    if (s_requests[index].answered) fail("request answered twice");
    job_t job = {.index = index};
    char *copy = NULL;
    if (body && result == XIAOTAI_OK) {
        size_t length = strlen(body);
        if (length >= FAKE_BODY_MAX) fail("body too long");
        /* Exactly the body and its terminator: reads past it are reported. */
        copy = malloc(length + 1U);
        if (!copy) fail("malloc");
        memcpy(copy, body, length + 1U);
        job.reply.body = copy;
        job.reply.size = length;
    }
    job.reply.result = result;
    job.reply.http_status = http_status;
    job.reply.code = XIAOTAI_PLATFORM_NO_CODE;
    job.reply.generation = generation;
    run_and_wait(answer_job, &job);
    free(copy);
}

void fake_platform_answer(size_t index, xiaotai_err_t result, int http_status, const char *body)
{
    if (index >= s_request_count) fail("answer to no request");
    fake_platform_answer_generation(index, result, http_status, body, s_requests[index].generation);
}

static void signal_job(void *ctx)
{
    job_t *job = ctx;
    char json[128];
    snprintf(json, sizeof(json), "{\"type\":\"%s\"}", job->type);
    xiaotai_platform_signal_t signal = {
        .type = job->type,
        .channel = "",
        .json = json,
        .size = strlen(json),
        .generation = xiaotai_identity_generation(),
        .command = false,
    };
    for (size_t i = 0; i < s_subscription_count; ++i) {
        if (strcmp(s_subscriptions[i].type, job->type) == 0) {
            s_subscriptions[i].handler(&signal, s_subscriptions[i].ctx);
        }
    }
    xiaotai_signal_give(&job->done);
}

void fake_platform_signal(const char *type)
{
    job_t job = {.type = type};
    run_and_wait(signal_job, &job);
}
