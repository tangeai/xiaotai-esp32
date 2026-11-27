// SPDX-License-Identifier: MIT
/* Validate the entire response, then walk borrowed JSON slices to count,
 * classify and copy the requested range. The owner holds the cache lock
 * during copying; the parser itself neither locks nor allocates. */
#include <string.h>
#include "contacts_internal.h"

/* ------------------------------------------------------------------ helpers */

static size_t utf8_length(unsigned char lead)
{
    if (lead < 0x80U) return 1;
    if (lead >= 0xC2U && lead <= 0xDFU) return 2;
    if (lead >= 0xE0U && lead <= 0xEFU) return 3;
    if (lead >= 0xF0U && lead <= 0xF4U) return 4;
    return 0;
}

static bool utf8_valid(const unsigned char *text, size_t width)
{
    if (!width) return false;
    for (size_t i = 1; i < width; ++i)
        if ((text[i] & 0xc0U) != 0x80U) return false;
    return !(width >= 3U && ((text[0] == 0xe0U && text[1] < 0xa0U) ||
        (text[0] == 0xedU && text[1] >= 0xa0U) || (text[0] == 0xf0U && text[1] < 0x90U) ||
        (text[0] == 0xf4U && text[1] >= 0x90U)));
}

xiaotai_err_t contacts_validate_remark(const char *remark)
{
    if (!remark) return XIAOTAI_OK;
    const size_t n = strnlen(remark, XIAOTAI_CONTACT_REMARK_BYTES_MAX + 1U);
    if (n > XIAOTAI_CONTACT_REMARK_BYTES_MAX) return XIAOTAI_ERR_INVALID_SIZE;
    size_t chars = 0;
    for (size_t i = 0; i < n;) {
        const size_t width = utf8_length((unsigned char)remark[i]);
        if (width > n - i || !utf8_valid((const unsigned char *)remark + i, width)) return XIAOTAI_ERR_INVALID_ARG;
        if (++chars > XIAOTAI_CONTACT_REMARK_CHARS_MAX) return XIAOTAI_ERR_INVALID_SIZE;
        i += width;
    }
    return XIAOTAI_OK;
}

void contacts_copy_utf8(char *dest, size_t capacity, const char *source)
{
    if (!dest || capacity == 0) return;
    size_t used = 0;
    while (source && *source && used + 1U < capacity) {
        size_t bytes = utf8_length((unsigned char)*source);
        if (bytes == 0 || used + bytes >= capacity) break;
        bool whole = true;
        for (size_t i = 1; i < bytes; ++i) {
            if (((unsigned char)source[i] & 0xC0U) != 0x80U) {
                whole = false;
                break;
            }
        }
        if (!whole) break;
        memcpy(dest + used, source, bytes);
        used += bytes;
        source += bytes;
    }
    dest[used] = '\0';
}

/* The single top-level "code" as an int32; false when absent, repeated, not
 * integral or out of range (the platform core's strict reading). */
static bool read_code(const xiaotai_json_view_t *root, int32_t *code)
{
    if (root->type != XIAOTAI_JSON_OBJECT) return false;
    bool found = false;
    int64_t value = 0;
    xiaotai_json_view_iter_t iter;
    xiaotai_json_view_iter(root, &iter);
    xiaotai_json_view_t key, member;
    while (xiaotai_json_view_next(&iter, &key, &member)) {
        if (!xiaotai_json_view_equals(&key, "code")) continue;
        if (found || xiaotai_json_view_int(&member, &value) != XIAOTAI_OK) return false;
        found = true;
    }
    if (!found || value <= (int64_t)INT32_MIN || value > (int64_t)INT32_MAX) return false;
    *code = (int32_t)value;
    return true;
}

/* A string member decoded into out (capacity with the terminator). 1 when
 * present and fitting, 0 when absent or not a string, -1 when it does not
 * fit or does not decode. */
static int read_string(const xiaotai_json_view_t *object, const char *key, char *out, size_t capacity)
{
    out[0] = '\0';
    xiaotai_json_view_t value;
    if (!xiaotai_json_view_get(object, key, &value) || value.type != XIAOTAI_JSON_STRING) return 0;
    if (xiaotai_json_view_string(&value, out, capacity, NULL) != XIAOTAI_OK) {
        out[0] = '\0';
        return -1;
    }
    return 1;
}

static int kind_of_type(const contacts_parse_in_t *in, const xiaotai_json_view_t *type,
                        const char *type_text)
{
    for (size_t k = 0; k < in->kind_count; ++k) {
        const char *server_type = in->kinds[k]->server_type;
        bool same = type_text ? strcmp(type_text, server_type) == 0 : xiaotai_json_view_equals(type, server_type);
        if (same) return (int)k;
    }
    return -1;
}

typedef enum {
    ENTRY_OK = 0,
    ENTRY_MALFORMED,       /* invalid fields after identifying a known kind */
    ENTRY_UNCLASSIFIED,    /* no usable type */
    ENTRY_UNREGISTERED,
} entry_t;

/* Classifies one element of data.contacts and, when it is good, fills
 * *contact. *kind receives its kind number. */
static entry_t read_entry(const contacts_parse_in_t *in, const xiaotai_json_view_t *item, int *kind,
                          xiaotai_contact_t *contact)
{
    if (item->type != XIAOTAI_JSON_OBJECT) return ENTRY_UNCLASSIFIED;
    const xiaotai_contacts_source_t *source = in->source;
    int k;
    if (source->type_field) {
        xiaotai_json_view_t type;
        if (!xiaotai_json_view_get(item, source->type_field, &type) || type.type != XIAOTAI_JSON_STRING)
            return ENTRY_UNCLASSIFIED;
        k = kind_of_type(in, &type, NULL);
    } else {
        k = kind_of_type(in, NULL, source->default_type);
    }
    if (k < 0) return ENTRY_UNREGISTERED;

    memset(contact, 0, sizeof(*contact));
    contact->kind = (int8_t)k;
    if (read_string(item, source->id_field, contact->id, sizeof(contact->id)) != 1 || !contact->id[0]) {
        return ENTRY_MALFORMED;
    }
    const xiaotai_contact_kind_def_t *def = in->kinds[k];
    for (size_t i = 0; i < XIAOTAI_CONTACT_EXTRAS; ++i) {
        if (!def->extra_fields[i]) continue;
        int got = read_string(item, def->extra_fields[i], contact->extra[i], sizeof(contact->extra[i]));
        const bool required = (def->extra_required & (1U << i)) != 0;
        if (got < 0 && (!in->reject_invalid_known_rows || required)) return ENTRY_MALFORMED;
        if (required && !contact->extra[i][0]) return ENTRY_MALFORMED;
    }

    /* The remark, else the name the server sends ("name", or "device_name" as
     * the list of a user's own devices spells it), else "". A name that does
     * not decode or exceeds a remark's length counts as absent. */
    static const char *const fields[] = {"remark", "name", "device_name"};
    char name[CONTACTS_NAME_READ_MAX + 1U] = "";
    for (size_t i = 0; !name[0] && i < sizeof(fields) / sizeof(fields[0]); ++i) {
        if (read_string(item, fields[i], name, sizeof(name)) != 1) name[0] = '\0';
    }
    contacts_copy_utf8(contact->name, sizeof(contact->name), name);

    if (def->presence) {
        xiaotai_json_view_t online;
        bool value = false;
        if (xiaotai_json_view_get(item, "online", &online) &&
            xiaotai_json_view_bool(&online, &value) == XIAOTAI_OK) {
            contact->online_known = true;
            contact->online = value;
        }
    }
    *kind = k;
    return ENTRY_OK;
}

/* Start of the cache range in the list filtered for the selection: the whole
 * list when it fits, otherwise a window around the selected page (clamped to
 * the last page), which always contains that page. */
static uint16_t range_start(const contacts_parse_in_t *in, uint16_t filtered)
{
    if (filtered <= in->capacity) return 0;
    uint16_t size = in->page_size;
    uint16_t pages = (uint16_t)((filtered + size - 1U) / size);
    uint16_t page = in->page < pages ? in->page : (uint16_t)(pages - 1U);
    int32_t start = (int32_t)page * size;
    int32_t offset = start - (int32_t)(in->capacity - size) / 2;
    int32_t last = (int32_t)filtered - (int32_t)in->capacity;
    if (offset > last) offset = last;
    if (offset < 0) offset = 0;
    return (uint16_t)offset;
}

xiaotai_err_t contacts_query_remark(const contacts_parse_in_t *input, const contacts_reply_t *reply, int kind, size_t index,
                                    char *out, size_t capacity, size_t *length)
{
    xiaotai_json_view_iter_t iter;
    xiaotai_json_view_iter(&reply->contacts, &iter);
    xiaotai_json_view_t item;
    while (xiaotai_json_view_next(&iter, NULL, &item)) {
        xiaotai_contact_t contact;
        int entry_kind;
        if (read_entry(input, &item, &entry_kind, &contact) != ENTRY_OK ||
            (kind != XIAOTAI_CONTACT_ANY && entry_kind != kind)) continue;
        if (index-- != 0U) continue;
        xiaotai_json_view_t remark;
        (void)xiaotai_json_view_get(&item, "remark", &remark);
        size_t measured = 0;
        const xiaotai_err_t err = xiaotai_json_view_string(&remark, out, capacity, &measured);
        if (length) *length = measured;
        if (!measured && (err == XIAOTAI_OK || err == XIAOTAI_ERR_INVALID_SIZE)) return XIAOTAI_ERR_NOT_FOUND;
        return err == XIAOTAI_ERR_INVALID_ARG ? XIAOTAI_ERR_NOT_FOUND : err;
    }
    return XIAOTAI_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------- public */

xiaotai_err_t contacts_parse_envelope(size_t capacity, int http_status, const char *body, size_t size,
                                     contacts_reply_t *reply)
{
    memset(reply, 0, sizeof(*reply));
    reply->code = INT32_MIN;
    if (http_status != 200 || !body) return XIAOTAI_FAIL;
    size_t count = 0;
    const xiaotai_err_t err = xiaotai_json_view_parse(body, size, &count, &reply->root);
    if (err == XIAOTAI_OK || err == XIAOTAI_ERR_INVALID_SIZE) {
        /* Keep the old precedence for too many tokens or a depth-limit error.
         * Other syntax errors still take precedence over the token count. */
        if (err == XIAOTAI_OK && count <= capacity) {
            reply->body_size = size;
            return read_code(&reply->root, &reply->code) ? XIAOTAI_OK : XIAOTAI_FAIL;
        }
        return count > capacity ? XIAOTAI_ERR_INVALID_SIZE : XIAOTAI_FAIL;
    }
    return XIAOTAI_FAIL;
}

bool contacts_reply_array(const contacts_reply_t *reply, const char *name, xiaotai_json_view_t *array)
{
    xiaotai_json_view_t data;
    return xiaotai_json_view_get(&reply->root, "data", &data) &&
           xiaotai_json_view_get(&data, name, array) && array->type == XIAOTAI_JSON_ARRAY;
}

xiaotai_err_t contacts_parse_reply(const contacts_parse_in_t *in, int http_status, const char *body, size_t size,
                                   contacts_reply_t *reply)
{
    const xiaotai_err_t err = contacts_parse_envelope(in->token_capacity, http_status, body, size, reply);
    if (err != XIAOTAI_OK) return err;
    if (reply->code != in->source->success_code) return XIAOTAI_FAIL;
    return contacts_reply_array(reply, "contacts", &reply->contacts) ? XIAOTAI_OK : XIAOTAI_FAIL;
}

xiaotai_err_t contacts_parse_fill(const contacts_parse_in_t *in, const contacts_reply_t *reply, contacts_parse_out_t *out)
{
    memset(out, 0, sizeof(*out));
    xiaotai_json_view_iter_t iter;
    xiaotai_json_view_t item;
    xiaotai_contact_t entry;
    int kind = 0;
    size_t firsts_needed = 0;

    /* Pass 1: totals, skipped entries, the first entry of every kind. */
    xiaotai_json_view_iter(&reply->contacts, &iter);
    while (xiaotai_json_view_next(&iter, NULL, &item)) {
        entry_t result = read_entry(in, &item, &kind, &entry);
        if (result != ENTRY_OK) {
            if (in->reject_invalid_known_rows && result == ENTRY_MALFORMED) {
                memset(out, 0, sizeof(*out));
                return XIAOTAI_ERR_INVALID_RESPONSE;
            }
            if (out->skipped < UINT16_MAX) ++out->skipped;
            if (result == ENTRY_UNREGISTERED && out->unregistered < UINT16_MAX) ++out->unregistered;
            continue;
        }
        if (out->total == UINT16_MAX) continue;
        ++out->total;
        if (++out->totals[kind] == 1) {
            if (in->reject_invalid_known_rows) ++firsts_needed;
            else {
                out->first[kind] = true;
                in->firsts[kind] = entry;
            }
        }
    }

    /* The range: everything when the whole list fits, otherwise the selected
     * kind around the selected page. */
    int cache_kind = XIAOTAI_CONTACT_ANY;
    uint16_t filtered = out->total;
    if (out->total > in->capacity && in->kind != XIAOTAI_CONTACT_ANY) {
        cache_kind = in->kind;
        filtered = out->totals[in->kind];
    }
    uint16_t offset = range_start(in, filtered);
    out->cache_kind = (int8_t)cache_kind;
    out->cache_offset = offset;

    /* Pass 2: copy the range and the optional unfiltered prefix. Strict
     * validation also deferred the per-kind first entries. */
    uint16_t position = 0;
    xiaotai_json_view_iter(&reply->contacts, &iter);
    while ((out->cached < in->capacity || firsts_needed || out->dial_prefix_count < in->dial_prefix_capacity) &&
           xiaotai_json_view_next(&iter, NULL, &item)) {
        if (read_entry(in, &item, &kind, &entry) != ENTRY_OK) continue;
        if (out->dial_prefix_count < in->dial_prefix_capacity) {
            in->dial_prefix[out->dial_prefix_count++] = entry;
        }
        if (firsts_needed && out->totals[kind] && !out->first[kind]) {
            out->first[kind] = true;
            in->firsts[kind] = entry;
            --firsts_needed;
        }
        if (out->cached == in->capacity) continue;
        if (cache_kind != XIAOTAI_CONTACT_ANY && kind != cache_kind) continue;
        if (position++ < offset) continue;
        in->cache[out->cached++] = entry;
    }
    out->dropped = (uint16_t)(out->total - out->cached);
    return XIAOTAI_OK;
}

xiaotai_err_t contacts_parse_list(const contacts_parse_in_t *in, int http_status, const char *body, size_t size,
                                  contacts_parse_out_t *out)
{
    contacts_reply_t reply;
    xiaotai_err_t err = contacts_parse_reply(in, http_status, body, size, &reply);
    if (err != XIAOTAI_OK) {
        memset(out, 0, sizeof(*out));
        return err;
    }
    return contacts_parse_fill(in, &reply, out);
}
