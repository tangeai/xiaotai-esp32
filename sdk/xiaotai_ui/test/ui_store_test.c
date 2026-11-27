// SPDX-License-Identifier: MIT
/* View copies (src/ui_store.c): every member is copied and the copy no longer
 * depends on the caller's memory; strings are cut at a UTF-8 character
 * boundary at XIAOTAI_VIEW_TEXT_MAX and arrays at their limits, each cut
 * counted once; names that select something are kept whole or dropped; when
 * the arena is too small the members are kept in the retention order
 * (actions, fields, indicators, item labels, item details, body text last),
 * checked at every arena size from 0 up to the size that holds everything,
 * with arenas allocated to the exact size so the sanitizer sees any write
 * past them; a toast keeps only id and text; identity is (kind, custom, id);
 * the change bits name exactly the members that differ and equal content
 * gives none. */
#include <stdlib.h>
#include <string.h>
#include "ui_internal.h"
#include "ui_test.h"

static bool same(const char *a, const char *b)
{
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

/* `kept` is absent, or a prefix of `source` that ends on a character boundary. */
static bool prefix_of(const char *kept, const char *source)
{
    if (!kept) return true;
    if (!source) return false;
    size_t length = strlen(kept);
    if (strncmp(kept, source, length) != 0) return false;
    return ((uint8_t)source[length] & 0xC0U) != 0x80U;
}

/* A copy into an arena of exactly `capacity` bytes, owned by the caller. */
static uint32_t copy_into(ui_copy_t *copy, size_t capacity, const xiaotai_view_t *view, xiaotai_view_layer_t layer)
{
    memset(copy, 0, sizeof(*copy));
    copy->arena = malloc(capacity ? capacity : 1U);
    return ui_store_copy(copy, capacity, view, layer);
}

static bool inside(const ui_copy_t *copy, size_t capacity, const void *pointer)
{
    const uint8_t *p = pointer;
    return p >= copy->arena && p < copy->arena + capacity;
}

/* --------------------------------------------------------------- fixture */

#define ITEM_COUNT 10U

static char s_labels[ITEM_COUNT][32];
static char s_details[ITEM_COUNT][96];
static char s_text[512];
static xiaotai_view_item_t s_items[ITEM_COUNT];
static const char *const OPTIONS[3] = {"一分钟", "五分钟", "永不"};
static const xiaotai_view_action_t ACTIONS[2] = {
    {.label = "语音呼叫", .intent = "call.voice", .icon = "phone", .role = XIAOTAI_ROLE_PRIMARY, .value = 7},
    {.label = "返回", .intent = "nav.back", .icon = "back", .role = XIAOTAI_ROLE_BACK},
};
static const xiaotai_view_field_t FIELDS[2] = {
    {.kind = XIAOTAI_FIELD_STEPPER, .label = "音量", .intent = "settings.volume", .value = 5, .min = 0,
     .max = 10, .text = "格"},
    {.kind = XIAOTAI_FIELD_CHOICE, .label = "息屏", .intent = "settings.sleep", .value = 1, .options = OPTIONS,
     .option_count = 3},
};
static const xiaotai_view_indicator_t INDICATORS[2] = {
    {.icon = "wifi", .level = 2, .intent = "system.network"},
    {.icon = "mic_off", .level = 0},
};
static const uint8_t CUSTOM[24] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};

static xiaotai_view_t fixture(void)
{
    for (size_t i = 0; i < ITEM_COUNT; ++i) {
        snprintf(s_labels[i], sizeof(s_labels[i]), "联系人%zu", i);
        snprintf(s_details[i], sizeof(s_details[i]), "设备号详情：一二三四五六 %zu", i);
        s_items[i] = (xiaotai_view_item_t){.label = s_labels[i], .detail = s_details[i],
                                           .intent = "contacts.open", .value = (int32_t)i};
    }
    s_text[0] = '\0';
    while (strlen(s_text) + strlen("正文段落。") < sizeof(s_text)) strcat(s_text, "正文段落。");
    return (xiaotai_view_t){
        .kind = XIAOTAI_VIEW_LIST, .id = "contacts", .title = "通讯录", .subtitle = "设备", .status = "在线",
        .emotion = "neutral", .image = "qr", .text = s_text, .items = s_items, .item_count = ITEM_COUNT,
        .actions = ACTIONS, .action_count = 2, .fields = FIELDS, .field_count = 2, .indicators = INDICATORS,
        .indicator_count = 2, .custom_data = CUSTOM, .custom_size = sizeof(CUSTOM), .timer_s = 30,
        .timer_mode = XIAOTAI_TIMER_REMAINING, .focus = 3, .phase = XIAOTAI_TALK_SPEAKING, .level = 2,
        .speaker = XIAOTAI_SPEAKER_ASSISTANT, .progress = 40, .flags = XIAOTAI_VIEW_GRID, .duration_ms = 900,
        .display = 1};
}

/* ------------------------------------------------------------------ tests */

static int test_utf8_prefix(void)
{
    const char *text = "a中b";   /* 'a', 3 bytes, 'b' */
    CHECK(ui_store_utf8_prefix(text, 5, 5) == 5);
    CHECK(ui_store_utf8_prefix(text, 5, 9) == 5);
    CHECK(ui_store_utf8_prefix(text, 5, 4) == 4);
    CHECK(ui_store_utf8_prefix(text, 5, 3) == 1);
    CHECK(ui_store_utf8_prefix(text, 5, 2) == 1);
    CHECK(ui_store_utf8_prefix(text, 5, 1) == 1);
    CHECK(ui_store_utf8_prefix(text, 5, 0) == 0);
    return 0;
}

static int test_full_copy_is_independent(void)
{
    xiaotai_view_t view = fixture();
    ui_copy_t copy;
    const size_t capacity = 8192U;
    CHECK(copy_into(&copy, capacity, &view, XIAOTAI_LAYER_PAGE) == 0U);
    const xiaotai_view_t *out = &copy.view;
    CHECK(copy.present);
    CHECK(out->kind == XIAOTAI_VIEW_LIST && out->layer == XIAOTAI_LAYER_PAGE && out->display == 1);
    CHECK(out->timer_s == 30 && out->timer_mode == XIAOTAI_TIMER_REMAINING && out->focus == 3);
    CHECK(out->phase == XIAOTAI_TALK_SPEAKING && out->level == 2 && out->speaker == XIAOTAI_SPEAKER_ASSISTANT);
    CHECK(out->progress == 40 && out->flags == XIAOTAI_VIEW_GRID && out->duration_ms == 900);
    CHECK(same(out->id, "contacts") && same(out->title, "通讯录") && same(out->subtitle, "设备"));
    CHECK(same(out->status, "在线") && same(out->emotion, "neutral") && same(out->image, "qr"));
    CHECK(same(out->text, s_text) && out->custom == NULL);
    CHECK(out->custom_size == sizeof(CUSTOM) && memcmp(out->custom_data, CUSTOM, sizeof(CUSTOM)) == 0);
    CHECK(out->action_count == 2 && out->field_count == 2 && out->indicator_count == 2);
    CHECK(out->item_count == ITEM_COUNT);
    for (size_t i = 0; i < 2; ++i) {
        CHECK(same(out->actions[i].label, ACTIONS[i].label) && same(out->actions[i].intent, ACTIONS[i].intent));
        CHECK(same(out->actions[i].icon, ACTIONS[i].icon) && out->actions[i].role == ACTIONS[i].role);
        CHECK(out->actions[i].value == ACTIONS[i].value);
        CHECK(same(out->fields[i].label, FIELDS[i].label) && out->fields[i].value == FIELDS[i].value);
        CHECK(same(out->indicators[i].icon, INDICATORS[i].icon));
        CHECK(out->indicators[i].level == INDICATORS[i].level);
    }
    CHECK(same(out->fields[0].text, "格") && out->fields[0].options == NULL && out->fields[0].max == 10);
    CHECK(out->fields[1].option_count == 3 && same(out->fields[1].options[2], "永不"));
    for (size_t i = 0; i < ITEM_COUNT; ++i) {
        CHECK(same(out->items[i].label, s_labels[i]) && same(out->items[i].detail, s_details[i]));
        CHECK(same(out->items[i].intent, "contacts.open") && out->items[i].value == (int32_t)i);
    }
    /* Everything lives in the arena: the caller may reuse its memory at once. */
    CHECK(inside(&copy, capacity, out->id) && inside(&copy, capacity, out->text));
    CHECK(inside(&copy, capacity, out->items) && inside(&copy, capacity, out->items[9].detail));
    CHECK(inside(&copy, capacity, out->actions) && inside(&copy, capacity, out->fields[1].options));
    CHECK(inside(&copy, capacity, out->fields[1].options[0]) && inside(&copy, capacity, out->custom_data));
    CHECK(inside(&copy, capacity, out->indicators) && inside(&copy, capacity, out->indicators[0].intent));
    memset(s_labels, 'x', sizeof(s_labels));
    memset(s_text, 'y', sizeof(s_text) - 1U);
    s_items[0].label = "changed";
    CHECK(strncmp(out->items[0].label, "联系人0", strlen("联系人0") + 1U) == 0);
    CHECK(strncmp(out->text, "正文段落。", strlen("正文段落。")) == 0);
    free(copy.arena);
    return 0;
}

static int test_limits(void)
{
    /* Strings: 2047 bytes and the terminator fit; one more byte is cut, at a
     * character boundary (3-byte characters: 2046 bytes kept). */
    static char ascii[XIAOTAI_VIEW_TEXT_MAX + 8];
    static char wide[3 * 1000 + 1];
    memset(ascii, 'a', XIAOTAI_VIEW_TEXT_MAX - 1U);
    ascii[XIAOTAI_VIEW_TEXT_MAX - 1U] = '\0';
    for (size_t i = 0; i < 1000; ++i) memcpy(wide + 3 * i, "中", 3);
    wide[3000] = '\0';

    ui_copy_t copy;
    xiaotai_view_t view = {.kind = XIAOTAI_VIEW_NOTICE, .title = ascii, .text = wide};
    CHECK(copy_into(&copy, 16384U, &view, XIAOTAI_LAYER_PAGE) == 1U);
    CHECK(strlen(copy.view.title) == XIAOTAI_VIEW_TEXT_MAX - 1U);
    CHECK(strlen(copy.view.text) == 2046U && prefix_of(copy.view.text, wide));
    free(copy.arena);

    ascii[XIAOTAI_VIEW_TEXT_MAX - 1U] = 'a';
    ascii[XIAOTAI_VIEW_TEXT_MAX] = '\0';
    view.text = NULL;
    CHECK(copy_into(&copy, 16384U, &view, XIAOTAI_LAYER_PAGE) == 1U);
    CHECK(strlen(copy.view.title) == XIAOTAI_VIEW_TEXT_MAX - 1U);
    free(copy.arena);

    /* An over-long name is dropped, never cut. */
    view = (xiaotai_view_t){.kind = XIAOTAI_VIEW_NOTICE, .id = ascii, .emotion = ascii};
    CHECK(copy_into(&copy, 16384U, &view, XIAOTAI_LAYER_PAGE) == 2U);
    CHECK(copy.view.id == NULL && copy.view.emotion == NULL);
    free(copy.arena);

    /* Arrays at their limits, each counted once. */
    static xiaotai_view_item_t items[40];
    static xiaotai_view_action_t actions[9];
    static xiaotai_view_field_t fields[XIAOTAI_VIEW_FIELDS_MAX + 1U];
    static xiaotai_view_indicator_t indicators[5];
    static const char *options[10] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    static uint8_t custom[600];
    for (size_t i = 0; i < 40; ++i) items[i] = (xiaotai_view_item_t){.label = "x", .value = (int32_t)i};
    for (size_t i = 0; i < 9; ++i) actions[i] = (xiaotai_view_action_t){.label = "a", .value = (int32_t)i};
    for (size_t i = 0; i < XIAOTAI_VIEW_FIELDS_MAX + 1U; ++i) fields[i] = (xiaotai_view_field_t){.kind = XIAOTAI_FIELD_SWITCH};
    fields[0] = (xiaotai_view_field_t){.kind = XIAOTAI_FIELD_CHOICE, .options = options, .option_count = 10};
    fields[1] = (xiaotai_view_field_t){.kind = XIAOTAI_FIELD_DIGITS, .max = 20, .text = "12345678901234567890"};
    for (size_t i = 0; i < sizeof(custom); ++i) custom[i] = (uint8_t)i;
    view = (xiaotai_view_t){.kind = XIAOTAI_VIEW_FORM, .items = items, .item_count = 40, .actions = actions,
                            .action_count = 9, .fields = fields, .field_count = XIAOTAI_VIEW_FIELDS_MAX + 1U, .indicators = indicators,
                            .indicator_count = 5, .custom_data = custom, .custom_size = sizeof(custom)};
    /* items, actions, fields, indicators, options, custom_data, DIGITS max and DIGITS text */
    CHECK(copy_into(&copy, 16384U, &view, XIAOTAI_LAYER_PAGE) == 8U);
    CHECK(copy.view.item_count == XIAOTAI_VIEW_ITEMS_MAX && copy.view.items[31].value == 31);
    CHECK(copy.view.action_count == XIAOTAI_VIEW_ACTIONS_MAX && copy.view.actions[7].value == 7);
    CHECK(copy.view.field_count == XIAOTAI_VIEW_FIELDS_MAX && copy.view.indicator_count == 4);
    CHECK(copy.view.fields[0].option_count == XIAOTAI_VIEW_OPTIONS_MAX && same(copy.view.fields[0].options[7], "7"));
    CHECK(copy.view.fields[1].max == (int32_t)XIAOTAI_VIEW_DIGITS_MAX);
    CHECK(same(copy.view.fields[1].text, "1234567890123456"));
    CHECK(copy.view.custom_size == XIAOTAI_VIEW_CUSTOM_MAX && memcmp(copy.view.custom_data, custom, 512) == 0);
    free(copy.arena);

    /* A NULL array with a count is empty, not a fault. */
    view = (xiaotai_view_t){.kind = XIAOTAI_VIEW_LIST, .item_count = 5, .action_count = 3, .custom_size = 9};
    CHECK(copy_into(&copy, 1024U, &view, XIAOTAI_LAYER_PAGE) == 0U);
    CHECK(copy.view.item_count == 0 && copy.view.action_count == 0 && copy.view.custom_size == 0);
    free(copy.arena);
    return 0;
}

/* Completeness of each group of a copy against the fixture. */
typedef struct {
    bool actions, fields, indicators, labels, details, text, header, custom;
    bool any_field, any_indicator, any_item, any_detail, any_text;
    size_t kept;   /* bytes and elements kept, printed on a failure */
} kept_t;

static int measure(const xiaotai_view_t *out, kept_t *k)
{
    memset(k, 0, sizeof(*k));
    k->header = same(out->id, "contacts") && same(out->title, "通讯录") && same(out->subtitle, "设备") &&
                same(out->status, "在线") && same(out->emotion, "neutral") && same(out->image, "qr");
    k->custom = out->custom_size == sizeof(CUSTOM);
    k->actions = out->action_count == 2;
    for (size_t i = 0; i < out->action_count; ++i) {
        CHECK(prefix_of(out->actions[i].label, ACTIONS[i].label));
        CHECK(!out->actions[i].intent || same(out->actions[i].intent, ACTIONS[i].intent));
        CHECK(!out->actions[i].icon || same(out->actions[i].icon, ACTIONS[i].icon));
        k->actions = k->actions && same(out->actions[i].label, ACTIONS[i].label) &&
                     same(out->actions[i].intent, ACTIONS[i].intent) && same(out->actions[i].icon, ACTIONS[i].icon);
        k->kept += 1U + (out->actions[i].label ? strlen(out->actions[i].label) : 0U) +
                   (out->actions[i].intent != NULL) + (out->actions[i].icon != NULL);
    }
    k->any_field = out->field_count > 0;
    k->fields = out->field_count == 2;
    for (size_t i = 0; i < out->field_count; ++i) {
        CHECK(prefix_of(out->fields[i].label, FIELDS[i].label) && prefix_of(out->fields[i].text, FIELDS[i].text));
        CHECK(!out->fields[i].intent || same(out->fields[i].intent, FIELDS[i].intent));
        k->fields = k->fields && same(out->fields[i].label, FIELDS[i].label) &&
                    same(out->fields[i].intent, FIELDS[i].intent) && same(out->fields[i].text, FIELDS[i].text) &&
                    out->fields[i].option_count == FIELDS[i].option_count;
        for (size_t o = 0; o < out->fields[i].option_count; ++o) {
            CHECK(prefix_of(out->fields[i].options[o], OPTIONS[o]));
            k->fields = k->fields && same(out->fields[i].options[o], OPTIONS[o]);
        }
        k->kept += 1U + out->fields[i].option_count + (out->fields[i].label ? strlen(out->fields[i].label) : 0U);
    }
    k->any_indicator = out->indicator_count > 0;
    k->indicators = out->indicator_count == 2;
    for (size_t i = 0; i < out->indicator_count; ++i) {
        CHECK(!out->indicators[i].icon || same(out->indicators[i].icon, INDICATORS[i].icon));
        k->indicators = k->indicators && same(out->indicators[i].icon, INDICATORS[i].icon) &&
                        same(out->indicators[i].intent, INDICATORS[i].intent);
        k->kept += 1U;
    }
    k->any_item = out->item_count > 0;
    k->labels = out->item_count == ITEM_COUNT;
    k->details = out->item_count == ITEM_COUNT;
    for (size_t i = 0; i < out->item_count; ++i) {
        CHECK(prefix_of(out->items[i].label, s_labels[i]) && prefix_of(out->items[i].detail, s_details[i]));
        CHECK(!out->items[i].intent || same(out->items[i].intent, "contacts.open"));
        k->labels = k->labels && same(out->items[i].label, s_labels[i]) && same(out->items[i].intent, "contacts.open");
        k->details = k->details && same(out->items[i].detail, s_details[i]);
        k->any_detail = k->any_detail || out->items[i].detail != NULL;
        k->kept += 1U + (out->items[i].label ? strlen(out->items[i].label) : 0U) +
                   (out->items[i].detail ? strlen(out->items[i].detail) : 0U);
    }
    CHECK(prefix_of(out->text, s_text));
    k->any_text = out->text != NULL;
    k->text = same(out->text, s_text);
    k->kept += out->text ? strlen(out->text) : 0U;
    return 0;
}

static int test_retention_order(void)
{
    const xiaotai_view_t view = fixture();
    bool complete = false;
    size_t full = 0U;
    for (size_t capacity = 0; capacity <= 8192U && !complete; ++capacity) {
        ui_copy_t copy;
        uint32_t cuts = copy_into(&copy, capacity, &view, XIAOTAI_LAYER_PAGE);
        kept_t k;
        if (measure(&copy.view, &k) != 0) {
            fprintf(stderr, "capacity %zu, kept %zu\n", capacity, k.kept);
            return 1;
        }
        /* Each later group only once every earlier one is whole. */
        if (k.custom || k.actions) CHECK(k.header);
        if (k.any_field) CHECK(k.header && k.actions);
        if (k.any_indicator) CHECK(k.fields);
        if (k.any_item) CHECK(k.fields && k.indicators);
        if (k.any_detail) CHECK(k.labels);
        if (k.any_text) CHECK(k.details);
        complete = k.header && k.custom && k.actions && k.fields && k.indicators && k.labels && k.details && k.text;
        CHECK((cuts == 0U) == complete);
        if (complete) full = capacity;
        free(copy.arena);
    }
    CHECK(complete && full > 0U);
    /* One byte short of the whole view cuts the end of the body text only. */
    ui_copy_t copy;
    CHECK(copy_into(&copy, full - 1U, &view, XIAOTAI_LAYER_PAGE) == 1U);
    kept_t k;
    CHECK(measure(&copy.view, &k) == 0);
    CHECK(k.details && k.any_text && !k.text && prefix_of(copy.view.text, s_text));
    free(copy.arena);
    return 0;
}

static int test_toast_copy(void)
{
    static char text[400];
    text[0] = '\0';
    while (strlen(text) + 3U < sizeof(text)) strcat(text, "保");
    xiaotai_view_t view = {.kind = XIAOTAI_VIEW_NOTICE, .layer = XIAOTAI_LAYER_TOAST, .id = "saved", .text = text,
                           .title = "ignored", .actions = ACTIONS, .action_count = 2, .duration_ms = 1500};
    ui_copy_t copy;
    /* id (6 bytes) and as much text as fits in the toast's 192 bytes. */
    CHECK(copy_into(&copy, XIAOTAI_UI_TOAST_BYTES, &view, XIAOTAI_LAYER_TOAST) == 1U);
    CHECK(same(copy.view.id, "saved") && copy.view.title == NULL && copy.view.action_count == 0);
    CHECK(copy.view.duration_ms == 1500 && copy.view.layer == XIAOTAI_LAYER_TOAST);
    CHECK(prefix_of(copy.view.text, text) && strlen(copy.view.text) == 183U);   /* 185 bytes left, 61 characters */
    free(copy.arena);
    return 0;
}

static int test_identity(void)
{
    ui_copy_t copy;
    xiaotai_view_t a = {.kind = XIAOTAI_VIEW_CUSTOM, .custom = "door.home", .id = "front", .title = "x"};
    CHECK(copy_into(&copy, 1024U, &a, XIAOTAI_LAYER_PAGE) == 0U);
    xiaotai_view_t b = a;
    b.title = "another title";
    CHECK(ui_store_same_identity(&b, 1024U, XIAOTAI_LAYER_PAGE, &copy.view));
    b.id = "back";
    CHECK(!ui_store_same_identity(&b, 1024U, XIAOTAI_LAYER_PAGE, &copy.view));
    b.id = NULL;
    CHECK(!ui_store_same_identity(&b, 1024U, XIAOTAI_LAYER_PAGE, &copy.view));
    b = a;
    b.custom = "door.call";
    CHECK(!ui_store_same_identity(&b, 1024U, XIAOTAI_LAYER_PAGE, &copy.view));
    b = a;
    b.kind = XIAOTAI_VIEW_NOTICE;
    CHECK(!ui_store_same_identity(&b, 1024U, XIAOTAI_LAYER_PAGE, &copy.view));
    free(copy.arena);

    /* NULL and "" are different ids. */
    xiaotai_view_t c = {.kind = XIAOTAI_VIEW_LIST, .id = ""};
    CHECK(copy_into(&copy, 64U, &c, XIAOTAI_LAYER_PAGE) == 0U);
    c.id = NULL;
    CHECK(!ui_store_same_identity(&c, 64U, XIAOTAI_LAYER_PAGE, &copy.view));
    free(copy.arena);

    /* An id that never fits the arena is dropped the same way every time, so
     * the view is still recognised as the same one. */
    static char long_id[100];
    memset(long_id, 'i', sizeof(long_id) - 1U);
    xiaotai_view_t d = {.kind = XIAOTAI_VIEW_LIST, .custom = "c", .id = long_id};
    CHECK(copy_into(&copy, 64U, &d, XIAOTAI_LAYER_PAGE) == 1U);
    CHECK(copy.view.id == NULL && same(copy.view.custom, "c"));
    CHECK(ui_store_same_identity(&d, 64U, XIAOTAI_LAYER_PAGE, &copy.view));
    d.id = NULL;
    CHECK(ui_store_same_identity(&d, 64U, XIAOTAI_LAYER_PAGE, &copy.view));
    free(copy.arena);

    /* A custom name that does not fit ends the copy: the id goes with it. */
    xiaotai_view_t e = {.kind = XIAOTAI_VIEW_CUSTOM, .custom = long_id, .id = "x"};
    CHECK(copy_into(&copy, 64U, &e, XIAOTAI_LAYER_PAGE) == 2U);
    CHECK(copy.view.custom == NULL && copy.view.id == NULL);
    CHECK(ui_store_same_identity(&e, 64U, XIAOTAI_LAYER_PAGE, &copy.view));
    e.id = "y";
    CHECK(ui_store_same_identity(&e, 64U, XIAOTAI_LAYER_PAGE, &copy.view));
    free(copy.arena);
    return 0;
}

/* The bits between a copy of `before` and a copy of `after`. */
static uint32_t diff_of(const xiaotai_view_t *before, const xiaotai_view_t *after)
{
    ui_copy_t a, b;
    copy_into(&a, 8192U, before, XIAOTAI_LAYER_PAGE);
    copy_into(&b, 8192U, after, XIAOTAI_LAYER_PAGE);
    uint32_t changed = ui_store_diff(&a.view, &b.view);
    free(a.arena);
    free(b.arena);
    return changed;
}

static int test_change_bits(void)
{
    const xiaotai_view_t base = fixture();
    xiaotai_view_t v;
    /* Equal content in other memory: nothing changed. */
    static char title_again[32];
    strcpy(title_again, "通讯录");
    v = base;
    v.title = title_again;
    v.duration_ms = 5;   /* not drawn */
    CHECK(diff_of(&base, &v) == 0U);

#define EXPECT(mutation, bit)                   \
    do {                                        \
        v = base;                               \
        mutation;                               \
        CHECK(diff_of(&base, &v) == (bit));     \
    } while (0)

    EXPECT(v.title = "通话", XIAOTAI_CHANGED_TITLE);
    EXPECT(v.subtitle = NULL, XIAOTAI_CHANGED_TITLE);
    EXPECT(v.status = "保存中", XIAOTAI_CHANGED_TITLE);
    EXPECT(v.text = "字幕", XIAOTAI_CHANGED_TEXT);
    EXPECT(v.speaker = XIAOTAI_SPEAKER_USER, XIAOTAI_CHANGED_TEXT);
    EXPECT(v.emotion = "sleepy", XIAOTAI_CHANGED_EMOTION);
    EXPECT(v.phase = XIAOTAI_TALK_LISTENING, XIAOTAI_CHANGED_EMOTION);
    EXPECT(v.level = 3, XIAOTAI_CHANGED_EMOTION);
    EXPECT(v.item_count = 9, XIAOTAI_CHANGED_ITEMS);
    EXPECT(v.timer_s = 29, XIAOTAI_CHANGED_TIMER);
    EXPECT(v.timer_mode = XIAOTAI_TIMER_ELAPSED, XIAOTAI_CHANGED_TIMER);
    EXPECT(v.progress = 41, XIAOTAI_CHANGED_PROGRESS);
    EXPECT(v.focus = 4, XIAOTAI_CHANGED_FOCUS);
    EXPECT(v.image = "logo", XIAOTAI_CHANGED_IMAGE);
    EXPECT(v.flags = XIAOTAI_VIEW_GRID | XIAOTAI_VIEW_MAY_SLEEP, XIAOTAI_CHANGED_FLAGS);
    EXPECT(v.indicator_count = 1, XIAOTAI_CHANGED_INDICATORS);
    EXPECT(v.custom_size = 23, XIAOTAI_CHANGED_CUSTOM);
    EXPECT((v.title = "x", v.text = "y"), XIAOTAI_CHANGED_TITLE | XIAOTAI_CHANGED_TEXT);

    /* Element members, one at a time. */
    xiaotai_view_item_t items[ITEM_COUNT];
    memcpy(items, s_items, sizeof(items));
    items[4].detail = "另一个详情";
    EXPECT(v.items = items, XIAOTAI_CHANGED_ITEMS);
    memcpy(items, s_items, sizeof(items));
    items[9].state = XIAOTAI_STATE_CHECKED;
    EXPECT(v.items = items, XIAOTAI_CHANGED_ITEMS);

    xiaotai_view_action_t actions[2];
    memcpy(actions, ACTIONS, sizeof(actions));
    actions[0].state = XIAOTAI_STATE_BUSY;
    EXPECT(v.actions = actions, XIAOTAI_CHANGED_ACTIONS);
    memcpy(actions, ACTIONS, sizeof(actions));
    actions[1].trigger = XIAOTAI_TRIGGER_HOLD;
    EXPECT(v.actions = actions, XIAOTAI_CHANGED_ACTIONS);

    xiaotai_view_field_t fields[2];
    memcpy(fields, FIELDS, sizeof(fields));
    fields[0].value = 6;
    EXPECT(v.fields = fields, XIAOTAI_CHANGED_FIELDS);
    const char *options[3] = {"一分钟", "十分钟", "永不"};
    memcpy(fields, FIELDS, sizeof(fields));
    fields[1].options = options;
    EXPECT(v.fields = fields, XIAOTAI_CHANGED_FIELDS);

    xiaotai_view_indicator_t indicators[2];
    memcpy(indicators, INDICATORS, sizeof(indicators));
    indicators[0].level = -1;
    EXPECT(v.indicators = indicators, XIAOTAI_CHANGED_INDICATORS);

    uint8_t custom[sizeof(CUSTOM)];
    memcpy(custom, CUSTOM, sizeof(custom));
    custom[23] = 99;
    EXPECT(v.custom_data = custom, XIAOTAI_CHANGED_CUSTOM);
#undef EXPECT
    return 0;
}

int main(void)
{
    RUN(test_utf8_prefix);
    RUN(test_full_copy_is_independent);
    RUN(test_limits);
    RUN(test_retention_order);
    RUN(test_toast_copy);
    RUN(test_identity);
    RUN(test_change_bits);
    return 0;
}
