// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline bool wifi_credentials_valid(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) return false;
    size_t ssid_length = strlen(ssid);
    size_t password_length = strlen(password);
    return ssid_length > 0 && ssid_length <= 32 &&
           (password_length == 0 || (password_length >= 8 && password_length <= 64));
}

static inline bool wifi_credentials_associated(const char *expected, const uint8_t *actual, size_t actual_size)
{
    return expected != NULL && actual != NULL && expected[0] && actual_size <= 32 &&
           strlen(expected) == actual_size && memcmp(expected, actual, actual_size) == 0;
}

static inline bool wifi_verification_expired(bool pending, int64_t deadline_us, int64_t now_us)
{
    return pending && deadline_us > 0 && now_us >= deadline_us;
}
