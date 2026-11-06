// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
#include "wifi_credentials.h"

int main(void)
{
    char ssid[34], password[66];
    memset(ssid, 's', sizeof(ssid));
    memset(password, 'p', sizeof(password));
    ssid[32] = 0; password[64] = 0;
    assert(wifi_credentials_valid(ssid, password));
    assert(wifi_credentials_valid("test", ""));
    assert(wifi_credentials_valid("test", "12345678"));
    assert(!wifi_credentials_valid(NULL, ""));
    assert(!wifi_credentials_valid("", ""));
    assert(!wifi_credentials_valid("test", NULL));
    assert(!wifi_credentials_valid("test", "1234567"));
    ssid[32] = 's'; ssid[33] = 0;
    assert(!wifi_credentials_valid(ssid, password));
    password[64] = 'p'; password[65] = 0;
    assert(!wifi_credentials_valid("test", password));
    assert(wifi_credentials_associated("test", (const uint8_t *)"test", 4));
    assert(!wifi_credentials_associated("test", (const uint8_t *)"test-2", 6));
    assert(!wifi_credentials_associated("test", (const uint8_t *)"tes", 3));
    assert(!wifi_credentials_associated("", (const uint8_t *)"", 0));
    assert(!wifi_verification_expired(false, 30000000, 40000000));
    assert(!wifi_verification_expired(true, 0, 40000000));
    assert(!wifi_verification_expired(true, 30000000, 29999999));
    assert(wifi_verification_expired(true, 30000000, 30000000));
    assert(wifi_verification_expired(true, 30000000, 30000001));
    assert(!wifi_verification_expired(true, 60000000, 30000001));
    puts("Wi-Fi credentials: byte limits, association identity and per-attempt deadline passed");
    return 0;
}
