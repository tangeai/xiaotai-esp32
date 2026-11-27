// SPDX-License-Identifier: MIT
/* The AI service's wire facts: the token reply, the session request and its
 * reply, and the classification of commands. Pure functions on the caller's
 * memory; text is read and written with the JSON library. */
#include <stdlib.h>
#include <string.h>
#include "ai_talk_internal.h"

/* Deployed firmware parsed with a library that skips every byte up to 0x20
 * around the value (a sender counting its terminator, a trailing newline), so
 * those are cut off here and the strict reader sees the value alone. */
static const char *trim(const char *text, size_t *size)
{
    while (*size && (unsigned char)text[0] <= 0x20U) {
        ++text;
        --*size;
    }
    while (*size && (unsigned char)text[*size - 1U] <= 0x20U) --*size;
    return text;
}

void ai_talk_fields(const xiaotai_json_view_t *object, const char *const *names,
                    xiaotai_json_view_t *fields, size_t count)
{
    memset(fields, 0, count * sizeof(*fields));
    if (object->type != XIAOTAI_JSON_OBJECT) return;
    size_t missing = count;
    xiaotai_json_view_iter_t it;
    xiaotai_json_view_iter(object, &it);
    xiaotai_json_view_t key, value;
    while (missing && xiaotai_json_view_next(&it, &key, &value)) {
        for (size_t i = 0; i < count; ++i) {
            if (fields[i].type || !xiaotai_json_view_equals(&key, names[i])) continue;
            fields[i] = value;
            --missing;
            break;
        }
    }
}

/* Deployed firmware compared numbers as doubles: 200, 200.0 and 2e2 are all
 * 200, 200.5 is not. */
bool ai_talk_number(const xiaotai_json_view_t *field, double *out)
{
    char text[32];
    const size_t length = field->length;
    if (field->type != XIAOTAI_JSON_NUMBER || !length || length >= sizeof(text)) return false;
    memcpy(text, field->text, length);
    text[length] = '\0';
    char *end = NULL;
    const double value = strtod(text, &end);
    if (end != text + length) return false;
    *out = value;
    return true;
}

static bool number_equals(const xiaotai_json_view_t *field, double expected)
{
    double value;
    return ai_talk_number(field, &value) && value == expected;
}

/* A string member shorter than capacity. Absent or not a string: "" unless
 * required; empty: refused when required. */
static bool copy_member(const xiaotai_json_view_t *field, char *out, size_t capacity, bool required)
{
    out[0] = '\0';
    if (field->type != XIAOTAI_JSON_STRING) return !required;
    size_t length = 0;
    if (xiaotai_json_view_string(field, out, capacity, &length) != XIAOTAI_OK) {
        out[0] = '\0';
        return false;
    }
    return length > 0 || !required;
}

xiaotai_err_t ai_talk_token_parse(int http_status, const char *body, size_t size, bool role_required,
                                  size_t capacity, ai_talk_credentials_t *out)
{
    if (http_status != 200 || !body || !capacity || !out) return XIAOTAI_FAIL;
    body = trim(body, &size);
    xiaotai_json_view_t root;
    size_t count;
    if (xiaotai_json_view_parse(body, size, &count, &root) != XIAOTAI_OK ||
        count > capacity || root.type != XIAOTAI_JSON_OBJECT) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    static const char *const response[] = {"code", "data"};
    static const char *const credentials[] = {"peer_id", "token", "role_id"};
    xiaotai_json_view_t envelope[2], fields[3];
    ai_talk_fields(&root, response, envelope, 2U);
    if (!number_equals(&envelope[0], 200.0) || envelope[1].type != XIAOTAI_JSON_OBJECT) return XIAOTAI_FAIL;
    ai_talk_fields(&envelope[1], credentials, fields, 3U);
    if (!copy_member(&fields[0], out->peer, sizeof(out->peer), true) ||
        !copy_member(&fields[1], out->token, sizeof(out->token), true) ||
        !copy_member(&fields[2], out->role, sizeof(out->role), role_required)) {
        return XIAOTAI_FAIL;
    }
    return XIAOTAI_OK;
}

const ai_talk_audio_t ai_talk_audio_g711 = {"g711a", "alaw", 8000U};
const ai_talk_audio_t ai_talk_audio_opus = {"opus", NULL, 16000U};

static void put_audio(xiaotai_json_writer_t *writer, const char *key, const ai_talk_audio_t *audio)
{
    xiaotai_json_object_begin(writer, key);
    xiaotai_json_put_string(writer, "codec", audio->codec);
    xiaotai_json_put_int(writer, "sample_rate", (int32_t)audio->rate_hz);
    xiaotai_json_put_int(writer, "channels", 1);
    xiaotai_json_object_end(writer);
}

xiaotai_err_t ai_talk_session_request(char *out, size_t capacity, const char *request_id, const char *device_id,
                                      const char *role, const ai_talk_audio_t *up, const ai_talk_audio_t *down,
                                      size_t *length)
{
    xiaotai_json_writer_t writer;
    xiaotai_json_writer_init(&writer, out, capacity);
    xiaotai_json_object_begin(&writer, NULL);
    xiaotai_json_put_string(&writer, "jsonrpc", "2.0");
    xiaotai_json_put_string(&writer, "id", request_id);
    xiaotai_json_put_string(&writer, "method", "start_session");
    xiaotai_json_object_begin(&writer, "params");
    xiaotai_json_put_string(&writer, "device_id", device_id);
    xiaotai_json_put_string(&writer, "role_id", role);
    put_audio(&writer, "input_audio", up);
    put_audio(&writer, "output_audio", down);
    xiaotai_json_object_end(&writer);
    xiaotai_json_object_end(&writer);
    return xiaotai_json_writer_end(&writer, length);
}

void ai_talk_message_parse(const char *text, size_t size, size_t capacity,
                           ai_talk_message_t *out)
{
    *out = (ai_talk_message_t){0};
    if (!text || !capacity) return;
    text = trim(text, &size);
    xiaotai_json_view_t root;
    size_t count;
    if (xiaotai_json_view_parse(text, size, &count, &root) != XIAOTAI_OK ||
        count > capacity || root.type != XIAOTAI_JSON_OBJECT) {
        return;
    }
    static const char *const names[] = {"id", "method", "params", "result", "error"};
    xiaotai_json_view_t fields[5];
    ai_talk_fields(&root, names, fields, 5U);
    out->id = fields[0];
    out->method = fields[1];
    out->params = fields[2];
    /* Any "method" member, even null, means it is not a reply (deployed
     * firmware looked the member up, not its type). */
    if (!out->method.type) {
        out->kind = AI_TALK_MESSAGE_REPLY;
        out->result = fields[3];
        out->has_error = fields[4].type != 0;
    } else if (out->method.type == XIAOTAI_JSON_STRING) {
        out->kind = AI_TALK_MESSAGE_METHOD;
    }
}

bool ai_talk_message_is(const ai_talk_message_t *message, const char *method)
{
    return message->kind == AI_TALK_MESSAGE_METHOD && xiaotai_json_view_equals(&message->method, method);
}

static bool audio_supported(const xiaotai_json_view_t *format, const ai_talk_audio_t *audio)
{
    if (format->type != XIAOTAI_JSON_OBJECT) return false;
    static const char *const names[] = {"codec", "sample_rate", "channels"};
    xiaotai_json_view_t fields[3];
    ai_talk_fields(format, names, fields, 3U);
    return (xiaotai_json_view_equals(&fields[0], audio->codec) ||
            (audio->alias && xiaotai_json_view_equals(&fields[0], audio->alias))) &&
           number_equals(&fields[1], (double)audio->rate_hz) && number_equals(&fields[2], 1.0);
}

ai_talk_handshake_t ai_talk_handshake_check(const ai_talk_message_t *message, const char *request_id,
                                            const ai_talk_audio_t *up, const ai_talk_audio_t *down)
{
    if (message->kind != AI_TALK_MESSAGE_REPLY || !request_id || !request_id[0] ||
        !xiaotai_json_view_equals(&message->id, request_id)) {
        return AI_TALK_HANDSHAKE_NONE;
    }
    if (message->has_error) return AI_TALK_HANDSHAKE_REFUSED;
    if (message->result.type != XIAOTAI_JSON_OBJECT) return AI_TALK_HANDSHAKE_UNSUPPORTED;
    static const char *const names[] = {"session_id", "input_audio", "output_audio"};
    xiaotai_json_view_t fields[3];
    ai_talk_fields(&message->result, names, fields, 3U);
    size_t length = 0;
    /* Decoded length only: no buffer, so the call reports INVALID_SIZE. */
    const bool named = fields[0].type == XIAOTAI_JSON_STRING &&
                       xiaotai_json_view_string(&fields[0], NULL, 0, &length) == XIAOTAI_ERR_INVALID_SIZE &&
                       length > 0;
    if (!named || !audio_supported(&fields[1], up) || !audio_supported(&fields[2], down)) {
        return AI_TALK_HANDSHAKE_UNSUPPORTED;
    }
    return AI_TALK_HANDSHAKE_ACCEPTED;
}
