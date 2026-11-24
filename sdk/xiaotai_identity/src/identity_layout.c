// SPDX-License-Identifier: MIT
/* Storage layouts of the device identity, exactly as deployed firmware keeps
 * them, so a device keeps its binding across the firmware change and a
 * rollback still reads what this firmware wrote:
 *   separate entries  "device_id", "secret", "client_id" strings
 *   record            one 322-byte blob "identity": device_id[65], device_key[257]
 * A further layout is one more classifier and one more pair of operation
 * builders here. */
#include <string.h>

#include "identity_internal.h"

/* The storage told what is stored (absent or too large): not a failure of
 * the storage itself. */
static bool shape_result(xiaotai_err_t result)
{
    return result == XIAOTAI_OK || result == XIAOTAI_ERR_NOT_FOUND || result == XIAOTAI_ERR_INVALID_SIZE;
}

/* Text of a field read into a buffer of capacity bytes is in 1..capacity-1
 * characters and terminated. */
static bool field_ok(xiaotai_err_t result, const char *text, size_t capacity)
{
    if (result != XIAOTAI_OK) return false;
    const size_t length = strnlen(text, capacity);
    return length > 0U && length < capacity;
}

static void zero_tail(char *text, size_t capacity)
{
    const size_t length = strnlen(text, capacity);
    if (length < capacity) memset(text + length, 0, capacity - length);
}

xiaotai_err_t xiaotai_identity_classify_strings(xiaotai_err_t device_id, xiaotai_err_t secret,
                                                xiaotai_err_t client_id, xiaotai_identity_t *identity,
                                                xiaotai_identity_state_t *state)
{
    xiaotai_err_t storage = XIAOTAI_OK;
    if (!shape_result(device_id)) {
        storage = device_id;
    } else if (!shape_result(secret)) {
        storage = secret;
    }
    const bool required = storage == XIAOTAI_OK &&
                          field_ok(device_id, identity->device_id, sizeof(identity->device_id)) &&
                          field_ok(secret, identity->device_key, sizeof(identity->device_key));
    if (required && !shape_result(client_id)) storage = client_id;

    if (storage != XIAOTAI_OK) {
        *state = XIAOTAI_IDENTITY_UNREADABLE;
    } else if (device_id == XIAOTAI_ERR_NOT_FOUND && secret == XIAOTAI_ERR_NOT_FOUND) {
        *state = XIAOTAI_IDENTITY_ABSENT;
    } else if (required && client_id == XIAOTAI_ERR_NOT_FOUND) {
        /* Older devices have no client id: the default applies. */
        memset(identity->client_id, 0, sizeof(identity->client_id));
        *state = XIAOTAI_IDENTITY_VALID;
    } else if (required && client_id == XIAOTAI_OK &&
               strnlen(identity->client_id, sizeof(identity->client_id)) < sizeof(identity->client_id)) {
        *state = XIAOTAI_IDENTITY_VALID;
    } else {
        /* Part of a save that lost power, or an entry out of range: unbound,
         * never a partial identity. */
        *state = XIAOTAI_IDENTITY_INCOMPLETE;
    }
    if (*state != XIAOTAI_IDENTITY_VALID) {
        xiaotai_identity_wipe(identity, sizeof(*identity));
    } else {
        /* The loaded copy is compared as whole fields: nothing may follow a
         * terminator. */
        zero_tail(identity->device_id, sizeof(identity->device_id));
        zero_tail(identity->device_key, sizeof(identity->device_key));
        zero_tail(identity->client_id, sizeof(identity->client_id));
    }
    return storage;
}

xiaotai_err_t xiaotai_identity_classify_record(xiaotai_err_t result, const uint8_t *record, size_t size,
                                               xiaotai_identity_t *identity, xiaotai_identity_state_t *state)
{
    xiaotai_identity_wipe(identity, sizeof(*identity));
    if (result == XIAOTAI_ERR_NOT_FOUND) {
        *state = XIAOTAI_IDENTITY_ABSENT;
        return XIAOTAI_OK;
    }
    *state = XIAOTAI_IDENTITY_UNREADABLE;
    if (result == XIAOTAI_ERR_INVALID_SIZE) return XIAOTAI_OK;
    if (result != XIAOTAI_OK) return result;
    if (size != XIAOTAI_IDENTITY_RECORD_BYTES || record == NULL) return XIAOTAI_OK;
    const uint8_t *id = record;
    const uint8_t *key = record + XIAOTAI_IDENTITY_RECORD_ID_BYTES;
    if (!memchr(id, 0, XIAOTAI_IDENTITY_RECORD_ID_BYTES) || !memchr(key, 0, XIAOTAI_IDENTITY_RECORD_KEY_BYTES) ||
        id[0] == 0U || key[0] == 0U) {
        return XIAOTAI_OK;
    }
    /* strcpy-like copies: bytes after a terminator are not part of the
     * identity and stay zero in the loaded copy. */
    memcpy(identity->device_id, id, strlen((const char *)id));
    memcpy(identity->device_key, key, strlen((const char *)key));
    *state = XIAOTAI_IDENTITY_VALID;
    return XIAOTAI_OK;
}

static xiaotai_err_t read_strings(const char *space, xiaotai_identity_t *identity, xiaotai_identity_state_t *state)
{
    xiaotai_identity_wipe(identity, sizeof(*identity));
    const xiaotai_err_t id = xiaotai_kv_get_str(space, XIAOTAI_IDENTITY_KEY_DEVICE_ID, identity->device_id,
                                                sizeof(identity->device_id), XIAOTAI_KV_WAIT_MAX_MS);
    /* Stop at a storage failure: another wait of up to XIAOTAI_KV_WAIT_MAX_MS
     * would not change the outcome. */
    xiaotai_err_t secret = XIAOTAI_ERR_NOT_FOUND;
    if (shape_result(id)) {
        secret = xiaotai_kv_get_str(space, XIAOTAI_IDENTITY_KEY_SECRET, identity->device_key,
                                    sizeof(identity->device_key), XIAOTAI_KV_WAIT_MAX_MS);
    }
    xiaotai_err_t client = XIAOTAI_ERR_NOT_FOUND;
    if (shape_result(id) && shape_result(secret) &&
        field_ok(id, identity->device_id, sizeof(identity->device_id)) &&
        field_ok(secret, identity->device_key, sizeof(identity->device_key))) {
        client = xiaotai_kv_get_str(space, XIAOTAI_IDENTITY_KEY_CLIENT_ID, identity->client_id,
                                    sizeof(identity->client_id), XIAOTAI_KV_WAIT_MAX_MS);
    }
    return xiaotai_identity_classify_strings(id, secret, client, identity, state);
}

static xiaotai_err_t read_record(const char *space, xiaotai_identity_t *identity, xiaotai_identity_state_t *state)
{
    uint8_t record[XIAOTAI_IDENTITY_RECORD_BYTES];
    size_t size = sizeof(record);
    const xiaotai_err_t result =
        xiaotai_kv_get(space, XIAOTAI_IDENTITY_KEY_RECORD, record, &size, XIAOTAI_KV_WAIT_MAX_MS);
    const xiaotai_err_t err = xiaotai_identity_classify_record(result, record, size, identity, state);
    xiaotai_identity_wipe(record, sizeof(record));
    return err;
}

xiaotai_err_t xiaotai_identity_layout_read(const xiaotai_identity_settings_t *settings, xiaotai_identity_t *identity,
                                           xiaotai_identity_state_t *state)
{
    if (settings->layout == XIAOTAI_IDENTITY_LAYOUT_RECORD) return read_record(settings->space, identity, state);
    return read_strings(settings->space, identity, state);
}

size_t xiaotai_identity_layout_save_ops(const xiaotai_identity_settings_t *settings, const xiaotai_identity_t *identity,
                                        uint8_t record[XIAOTAI_IDENTITY_RECORD_BYTES],
                                        xiaotai_kv_op_t ops[XIAOTAI_IDENTITY_OPS_MAX])
{
    memset(ops, 0, sizeof(*ops) * XIAOTAI_IDENTITY_OPS_MAX);
    if (settings->layout == XIAOTAI_IDENTITY_LAYOUT_RECORD) {
        memset(record, 0, XIAOTAI_IDENTITY_RECORD_BYTES);
        memcpy(record, identity->device_id, strnlen(identity->device_id, XIAOTAI_IDENTITY_DEVICE_ID_MAX));
        memcpy(record + XIAOTAI_IDENTITY_RECORD_ID_BYTES, identity->device_key,
               strnlen(identity->device_key, XIAOTAI_IDENTITY_DEVICE_KEY_MAX));
        ops[0] = (xiaotai_kv_op_t){.kind = XIAOTAI_KV_OP_SET, .key = XIAOTAI_IDENTITY_KEY_RECORD, .data = record,
                                   .size = XIAOTAI_IDENTITY_RECORD_BYTES};
        return 1U;
    }
    /* Deployed order: a power loss after device_id leaves a set that reads
     * as INCOMPLETE; the obsolete entry goes last, its absence is no error. */
    ops[0] = (xiaotai_kv_op_t){.kind = XIAOTAI_KV_OP_SET_STR, .key = XIAOTAI_IDENTITY_KEY_DEVICE_ID,
                               .data = identity->device_id};
    ops[1] = (xiaotai_kv_op_t){.kind = XIAOTAI_KV_OP_SET_STR, .key = XIAOTAI_IDENTITY_KEY_SECRET,
                               .data = identity->device_key};
    ops[2] = (xiaotai_kv_op_t){.kind = XIAOTAI_KV_OP_SET_STR, .key = XIAOTAI_IDENTITY_KEY_CLIENT_ID,
                               .data = identity->client_id};
    ops[3] = (xiaotai_kv_op_t){.kind = XIAOTAI_KV_OP_ERASE, .key = XIAOTAI_IDENTITY_KEY_ENDPOINT};
    return 4U;
}

size_t xiaotai_identity_layout_erase_ops(const xiaotai_identity_settings_t *settings,
                                         xiaotai_kv_op_t ops[XIAOTAI_IDENTITY_OPS_MAX])
{
    memset(ops, 0, sizeof(*ops) * XIAOTAI_IDENTITY_OPS_MAX);
    if (settings->layout == XIAOTAI_IDENTITY_LAYOUT_RECORD) {
        ops[0] = (xiaotai_kv_op_t){.kind = XIAOTAI_KV_OP_ERASE, .key = XIAOTAI_IDENTITY_KEY_RECORD};
        return 1U;
    }
    /* Everything deployed firmware keeps in this namespace, named one by one
     * instead of clearing the namespace, so a later owner's entries stay. */
    static const char *const keys[XIAOTAI_IDENTITY_OPS_MAX] = {
        XIAOTAI_IDENTITY_KEY_DEVICE_ID, XIAOTAI_IDENTITY_KEY_SECRET, XIAOTAI_IDENTITY_KEY_CLIENT_ID,
        XIAOTAI_IDENTITY_KEY_ENDPOINT};
    for (size_t i = 0; i < XIAOTAI_IDENTITY_OPS_MAX; ++i) {
        ops[i] = (xiaotai_kv_op_t){.kind = XIAOTAI_KV_OP_ERASE, .key = keys[i]};
    }
    return XIAOTAI_IDENTITY_OPS_MAX;
}
