// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/net.h"

typedef enum {
    NET_PORTAL_SUBMIT_NONE,
    NET_PORTAL_SUBMIT_PENDING,
    NET_PORTAL_SUBMIT_RESTART,
    NET_PORTAL_SUBMIT_SCAN,
    NET_PORTAL_SUBMIT_SAVED_MISS,
} net_portal_submit_reason_t;

typedef struct {
    net_portal_submit_reason_t reason;
    bool verify;
    bool restart;
} net_portal_submit_result_t;

bool net_portal_active(uint32_t token);
xiaotai_err_t net_portal_start(uint32_t ipv4);
xiaotai_err_t net_portal_stop(void);
xiaotai_err_t net_provision_portal_submit(const xiaotai_net_submission_t *submission, uint32_t token,
                                          net_portal_submit_result_t *result);
xiaotai_err_t net_provision_submit_checked(const xiaotai_net_submission_t *submission, uint32_t token,
                                           bool (*active)(uint32_t), net_portal_submit_result_t *result);

#define NET_PORTAL_BODY_MAX 512U
#define NET_PORTAL_TEXT_MAX (2U + 6U * XIAOTAI_NET_SSID_MAX + 1U)
#define NET_PORTAL_HEX_MAX (2U * XIAOTAI_NET_SSID_MAX + 1U)

/* A single JSON object, with string/bool values; input need not end in NUL. */
bool net_portal_json_credentials(const uint8_t *body, size_t size, xiaotai_net_submission_t *result);
/* A quoted UTF-8 display string and lossless lowercase byte encoding. */
void net_portal_json_ssid(const char *ssid, char text[NET_PORTAL_TEXT_MAX], char hex[NET_PORTAL_HEX_MAX]);
