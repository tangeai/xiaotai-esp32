// SPDX-License-Identifier: MIT
/* Fingerprint and default client id. The platform compares the fingerprint as
 * text, so each product keeps the spelling its devices were bound with; the
 * MAC comes from the caller (the network engine's station address), because
 * on some boards it is known only once the network is up and must never be
 * made up here. */
#include <stdio.h>
#include <string.h>

#include "identity_internal.h"
#include "xiaotai/port/sys.h"

/* Copies text when it fits with its terminator; otherwise leaves "" behind. */
static xiaotai_err_t put(const char *text, size_t length, char *out, size_t capacity)
{
    if (length >= capacity) {
        out[0] = '\0';
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    memcpy(out, text, length + 1U);
    return XIAOTAI_OK;
}

static void hex12(const uint8_t mac[XIAOTAI_IDENTITY_MAC_BYTES], bool upper, char out[13])
{
    static const char lower_digits[] = "0123456789abcdef";
    static const char upper_digits[] = "0123456789ABCDEF";
    const char *digits = upper ? upper_digits : lower_digits;
    for (size_t i = 0; i < XIAOTAI_IDENTITY_MAC_BYTES; ++i) {
        out[i * 2U] = digits[mac[i] >> 4];
        out[i * 2U + 1U] = digits[mac[i] & 0x0FU];
    }
    out[12] = '\0';
}

xiaotai_err_t xiaotai_identity_fingerprint(const uint8_t mac[XIAOTAI_IDENTITY_MAC_BYTES], char *out,
                                           size_t capacity)
{
    if (mac == NULL || out == NULL || capacity == 0U) return XIAOTAI_ERR_INVALID_ARG;
    /* Early firmware bound devices with the MAC as 12 upper-case digits for
     * client id and fingerprint. Only an exact match with this device's MAC
     * keeps that text; any other stored client id, or none, leaves the
     * configured spelling. */
    char compact[13];
    hex12(mac, true, compact);
    char stored[XIAOTAI_IDENTITY_CLIENT_ID_MAX + 1U];
    xiaotai_identity_stored_client_id(stored);
    if (strcmp(stored, compact) == 0) return put(compact, 12U, out, capacity);

    const bool upper = xiaotai_identity_settings()->fingerprint == XIAOTAI_FINGERPRINT_UPPER_COLON;
    hex12(mac, upper, compact);
    char text[XIAOTAI_IDENTITY_FINGERPRINT_MAX + 1U];
    for (size_t i = 0; i < XIAOTAI_IDENTITY_MAC_BYTES; ++i) {
        text[i * 3U] = compact[i * 2U];
        text[i * 3U + 1U] = compact[i * 2U + 1U];
        text[i * 3U + 2U] = i + 1U < XIAOTAI_IDENTITY_MAC_BYTES ? ':' : '\0';
    }
    return put(text, XIAOTAI_IDENTITY_FINGERPRINT_MAX, out, capacity);
}

xiaotai_err_t xiaotai_identity_client_id(const uint8_t mac[XIAOTAI_IDENTITY_MAC_BYTES], char *out, size_t capacity)
{
    if (mac == NULL || out == NULL || capacity == 0U) return XIAOTAI_ERR_INVALID_ARG;
    char text[XIAOTAI_IDENTITY_CLIENT_ID_MAX + 1U];
    xiaotai_identity_stored_client_id(text);
    if (text[0] != '\0') return put(text, strlen(text), out, capacity);

    const char *prefix = xiaotai_identity_settings()->client_prefix;
    if (prefix == NULL) {
        xiaotai_firmware_t firmware;
        xiaotai_firmware(&firmware);
        prefix = firmware.chip != NULL ? firmware.chip : "";
    }
    char digits[13];
    hex12(mac, false, digits);
    const int length = snprintf(text, sizeof(text), "%s-%s", prefix, digits);
    /* A chip name too long for the client id is a port defect, not a reason
     * to send a cut identity. */
    if (length < 0 || (size_t)length >= sizeof(text)) {
        out[0] = '\0';
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    return put(text, (size_t)length, out, capacity);
}
