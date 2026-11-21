// SPDX-License-Identifier: MIT
#include "ai_talk_internal.h"

#include <string.h>
#include "xiaotai/port/log.h"
#include "xiaotai/port/mem.h"

#define ORDER_SLOTS 8U

typedef struct {
    int32_t group;
    uint32_t sequence;
    xiaotai_ai_talk_speaker_t speaker; /* NONE marks a free slot */
} order_entry_t;

static struct {
    xiaotai_ai_talk_caption_t value;
    order_entry_t *order;
    unsigned next_order;
    int32_t group;
} s_caption;

void ai_talk_caption_reset(void)
{
    xiaotai_mem_free(s_caption.value.text);
    xiaotai_mem_free(s_caption.order);
    memset(&s_caption, 0, sizeof(s_caption));
}

void ai_talk_caption_start(size_t bytes)
{
    if (!bytes || s_caption.value.text) return;
    s_caption.value.text = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, bytes, 1);
    if (s_caption.value.text) {
        s_caption.order = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, ORDER_SLOTS, sizeof(*s_caption.order));
    }
    if (!s_caption.value.text || !s_caption.order) {
        XIAOTAI_LOGW("ai_talk", "caption storage unavailable");
        ai_talk_caption_reset();
        return;
    }
    s_caption.value.capacity = bytes;
}

void ai_talk_caption_begin(uint32_t session)
{
    char *text = s_caption.value.text;
    const size_t capacity = s_caption.value.capacity;
    s_caption.value = (xiaotai_ai_talk_caption_t){.session = session, .text = text, .capacity = capacity};
    if (text) text[0] = '\0';
    if (s_caption.order) memset(s_caption.order, 0, ORDER_SLOTS * sizeof(*s_caption.order));
    s_caption.group = -1;
    s_caption.next_order = 0;
}

uint32_t ai_talk_caption_revision(void) { return s_caption.value.revision; }

static bool accept_sequence(xiaotai_ai_talk_speaker_t speaker, int32_t group, uint32_t sequence)
{
    for (unsigned i = 0; i < ORDER_SLOTS; ++i) {
        order_entry_t *entry = &s_caption.order[i];
        if (entry->speaker == speaker && entry->group == group) {
            if (sequence <= entry->sequence) return false;
            entry->sequence = sequence;
            return true;
        }
    }
    s_caption.order[s_caption.next_order] = (order_entry_t){.speaker = speaker, .group = group, .sequence = sequence};
    s_caption.next_order = (s_caption.next_order + 1U) % ORDER_SLOTS;
    return true;
}

/* Raw Unicode comes from the JSON string as-is. Reject incomplete and invalid
 * units before accepting the caption or changing its sequence. */
static size_t utf8_bytes(const char *text, size_t size)
{
    if (!size) return 0;
    const uint8_t first = (uint8_t)text[0];
    if (first < 0x80U) return 1;
    if (first < 0xc2U || first > 0xf4U) return 0;
    const size_t n = first < 0xe0U ? 2U : first < 0xf0U ? 3U : 4U;
    if (size < n) return 0;
    const uint8_t second = (uint8_t)text[1];
    if ((first == 0xe0U && second < 0xa0U) || (first == 0xedU && second >= 0xa0U) ||
        (first == 0xf0U && second < 0x90U) || (first == 0xf4U && second >= 0x90U)) return 0;
    for (size_t i = 1; i < n; ++i) {
        if (((uint8_t)text[i] & 0xc0U) != 0x80U) return 0;
    }
    return n;
}

static const char *checked_text(const xiaotai_json_view_t *view, size_t *length)
{
    /* Length validation rejects bad escapes and NUL without allocating. */
    if (view->type != XIAOTAI_JSON_STRING || view->length < 2U ||
        xiaotai_json_view_string(view, NULL, 0, NULL) != XIAOTAI_ERR_INVALID_SIZE) return NULL;
    const char *raw = view->text + 1U;
    *length = view->length - 2U; /* The view also includes the quotes. */
    for (size_t i = 0; i < *length;) {
        size_t n = utf8_bytes(raw + i, *length - i);
        if (!n) return NULL;
        i += n;
    }
    return raw;
}

/* Decode one checked unit at a time using the same JSON decoder as other
 * messages. The small token view avoids a second caption-sized buffer. */
static size_t read_unit(const char *text, size_t size, char decoded[5], size_t *written)
{
    if (text[0] != '\\') {
        const size_t n = utf8_bytes(text, size);
        memcpy(decoded, text, n);
        *written = n;
        return n;
    }
    xiaotai_json_token_t token = {.type = XIAOTAI_JSON_STRING, .end = text[1] == 'u' ? 6U : 2U};
    const xiaotai_json_doc_t view = {.text = text, .length = size, .tokens = &token, .count = 1};
    if (xiaotai_json_string(&view, 0, decoded, 5, written) == XIAOTAI_OK) return token.end;
    token.end = 12U; /* complete, already validated surrogate pair */
    return xiaotai_json_string(&view, 0, decoded, 5, written) == XIAOTAI_OK ? token.end : 0U;
}

static size_t store_text(const char *raw, size_t length, char *out, size_t capacity, size_t previous,
                         bool *changed, bool *truncated)
{
    size_t size = 0;
    for (size_t i = 0; i < length;) {
        char unit[5];
        size_t n = 0;
        const size_t consumed = read_unit(raw + i, length - i, unit, &n);
        if (!consumed || n > capacity - size - 1U) {
            *truncated = true;
            break;
        }
        if (size + n > previous || memcmp(out + size, unit, n) != 0) *changed = true;
        memcpy(out + size, unit, n);
        size += n;
        i += consumed;
    }
    if (size != previous) *changed = true;
    out[size] = '\0';
    return size;
}

bool ai_talk_caption_apply(const xiaotai_json_view_t *params, xiaotai_ai_talk_speaker_t *speaker,
                           bool *final)
{
    xiaotai_ai_talk_caption_t *value = &s_caption.value;
    if (!value->text || !value->session) return false;
    enum {TYPE, SEQUENCE, UTTERANCE, TEXT, FINAL, MODE, EMOTION, FIELDS};
    static const char *const names[FIELDS] = {
        [TYPE] = "caption_type", [SEQUENCE] = "seq_num", [UTTERANCE] = "utterance_id", [TEXT] = "text",
        [FINAL] = "is_final", [MODE] = "mode", [EMOTION] = "emotion",
    };
    xiaotai_json_view_t fields[FIELDS];
    ai_talk_fields(params, names, fields, FIELDS);
    double type, number;
    if (!ai_talk_number(&fields[TYPE], &type)) return false;
    const bool has_sequence = fields[SEQUENCE].type != 0;
    uint32_t sequence = 0;
    if (has_sequence) {
        if (!ai_talk_number(&fields[SEQUENCE], &number) || !(number >= 0.0 && number <= UINT32_MAX) ||
            number != (uint32_t)number) return false;
        sequence = (uint32_t)number;
    }
    int32_t group = -1;
    if (ai_talk_number(&fields[UTTERANCE], &number) &&
        number >= 0.0 && number <= INT32_MAX && number == (int32_t)number) group = (int32_t)number;
    size_t raw_size = 0;
    const char *raw = checked_text(&fields[TEXT], &raw_size);
    if (!raw) return false;
    const xiaotai_ai_talk_speaker_t who = type == 1.0 ? XIAOTAI_AI_TALK_SPEAKER_AI : XIAOTAI_AI_TALK_SPEAKER_USER;
    if (group >= 0 && has_sequence && !accept_sequence(who, group, sequence)) return false;
    bool done = false;
    (void)xiaotai_json_view_bool(&fields[FINAL], &done);
    const bool new_turn = who != value->speaker || group != s_caption.group;
    const bool append = who == XIAOTAI_AI_TALK_SPEAKER_AI && !new_turn &&
                        ai_talk_number(&fields[MODE], &number) && number == 1.0;
    const size_t offset = append ? value->size : 0;
    bool truncated = append && value->truncated;
    bool changed = new_turn || done != value->final;
    if (!truncated) {
        value->size = offset + store_text(raw, raw_size, value->text + offset, value->capacity - offset,
                                          value->size - offset, &changed, &truncated);
    }
    char emotion[XIAOTAI_AI_TALK_EMOTION_MAX] = {0};
    if (who == XIAOTAI_AI_TALK_SPEAKER_AI) {
        size_t emotion_size = 0;
        const xiaotai_json_view_t *tag = &fields[EMOTION];
        const bool valid = checked_text(tag, &emotion_size) &&
                           xiaotai_json_view_string(tag, emotion, sizeof(emotion), NULL) == XIAOTAI_OK;
        if (!valid && !new_turn) {
            memcpy(emotion, value->emotion, sizeof(emotion));
        }
    }
    if (truncated != value->truncated || strcmp(emotion, value->emotion) != 0) changed = true;
    if (new_turn) ++value->paragraph;
    if (changed) ++value->revision;
    value->speaker = who;
    value->final = done;
    value->truncated = truncated;
    memcpy(value->emotion, emotion, sizeof(emotion));
    s_caption.group = group;
    *speaker = who;
    *final = done;
    return true;
}

bool xiaotai_ai_talk_caption_read(xiaotai_ai_talk_caption_t *caption)
{
    const xiaotai_ai_talk_caption_t *value = &s_caption.value;
    if (!caption || (!caption->text && caption->capacity) ||
        (caption->revision == value->revision && caption->session == value->session)) return false;
    char *out = caption->text;
    const size_t capacity = caption->capacity;
    *caption = *value;
    caption->text = out;
    caption->capacity = capacity;
    caption->size = 0;
    if (capacity) {
        while (caption->size < value->size) {
            const size_t n = utf8_bytes(value->text + caption->size, value->size - caption->size);
            if (!n || n > capacity - caption->size - 1U) break;
            caption->size += n;
        }
        if (caption->size) memcpy(out, value->text, caption->size);
        out[caption->size] = '\0';
        if (caption->size != value->size) caption->truncated = true;
    }
    return true;
}
