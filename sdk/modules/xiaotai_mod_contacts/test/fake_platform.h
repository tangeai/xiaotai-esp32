// SPDX-License-Identifier: MIT
#pragma once
/* A fake of the platform core for the contacts module's host tests: records
 * what the module declares and submits, and lets the test answer requests
 * and deliver signals on the owner task, with any result, HTTP status, body
 * and binding generation. Only the functions the module calls exist. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/platform.h"

typedef struct {
    char service[XIAOTAI_PLATFORM_SERVICE_NAME_MAX];
    char path[XIAOTAI_PLATFORM_PATH_MAX];
    xiaotai_platform_method_t method;
    xiaotai_platform_auth_t auth;
    uint32_t flags;
    uint32_t timeout_ms;
    bool has_body;
    char body[769];
    uint32_t generation;     /* xiaotai_identity_generation() at submission */
    xiaotai_platform_reply_fn_t reply;
    void *ctx;
    bool answered;
} fake_request_t;

void fake_platform_reset(void);

/* Requests accepted so far, in order. */
size_t fake_platform_request_count(void);
const fake_request_t *fake_platform_request(size_t index);
/* The next submission is refused with err (once). */
void fake_platform_refuse_next(xiaotai_err_t err);

/* Answer request `index` on the owner task and wait until its handler
 * returned. The reply's generation is the submission's. */
void fake_platform_answer(size_t index, xiaotai_err_t result, int http_status, const char *body);
/* The same with another generation (a reply of an earlier binding). */
void fake_platform_answer_generation(size_t index, xiaotai_err_t result, int http_status, const char *body,
                                     uint32_t generation);
/* Deliver a signal of this type to its subscribers on the owner task. */
void fake_platform_signal(const char *type);

/* What the module declared at init. */
xiaotai_platform_requirement_t fake_platform_requirement(void);
size_t fake_platform_requirements(void);
size_t fake_platform_service_count(void);
const char *fake_platform_service(size_t index);
size_t fake_platform_subscription_count(void);
const char *fake_platform_subscription(size_t index);
