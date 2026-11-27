// SPDX-License-Identifier: MIT
/* Bounded copies of views and their comparison.
 *
 * A copy keeps what the application showed within the limits of
 * xiaotai/view.h and the arena of its layer. When the arena cannot hold
 * everything, members are kept in a fixed order (see ui_store_copy() in
 * ui_internal.h): what the user operates first, then what the user reads,
 * and the long body text last. The comparison is member by member on the
 * copies, so a theme only redraws what differs and never sets equal text
 * again (the existing screens compared before every set for the same reason:
 * an equal label text still relaid and redrew the widget). */
#include <stdalign.h>
#include <string.h>
#include "ui_internal.h"

/* ------------------------------------------------------------------ arena */

typedef struct {
    uint8_t *base;
    size_t size;
    size_t used;
    uint32_t cuts;
} arena_t;

static size_t arena_free(const arena_t *arena, size_t align)
{
    size_t start = (arena->used + align - 1U) & ~(align - 1U);
    return start >= arena->size ? 0U : arena->size - start;
}

/* Something did not fit: nothing later is kept either, so a short string of a
 * later group never takes the room an earlier group lacked. */
static void arena_exhaust(arena_t *arena)
{
    ++arena->cuts;
    arena->used = arena->size;
}

/* Room for `count` elements of `element` bytes, or as many as fit (*count is
 * lowered and the cut counted). NULL when none fits or none is asked. */
static void *arena_array(arena_t *arena, size_t element, size_t align, size_t *count)
{
    if (*count == 0U) return NULL;
    const size_t fit = arena_free(arena, align) / element;
    const size_t start = (arena->used + align - 1U) & ~(align - 1U);
    const bool short_of_room = fit < *count;
    if (short_of_room) *count = fit;
    uint8_t *out = *count ? arena->base + start : NULL;
    if (*count) arena->used = start + *count * element;
    if (short_of_room) arena_exhaust(arena);
    return out;
}

size_t ui_store_utf8_prefix(const char *text, size_t length, size_t limit)
{
    if (length <= limit) return length;
    size_t cut = limit;
    /* text[cut] is the first byte left out; a continuation byte there means
     * the cut falls inside a character, whose lead byte is dropped too. */
    while (cut > 0U && ((uint8_t)text[cut] & 0xC0U) == 0x80U) --cut;
    return cut;
}

/* Bytes of `text` kept within `max_bytes` (terminator included); *cut when
 * the string is longer. Reads at most max_bytes bytes of it. */
static size_t bounded_length(const char *text, size_t max_bytes, bool *cut)
{
    size_t length = strnlen(text, max_bytes);
    *cut = length >= max_bytes;
    return *cut ? max_bytes - 1U : length;
}

/* A copy of a readable string: cut at a character boundary to the limit and
 * to the room left. NULL for NULL, and when not even the terminator fits. */
static const char *arena_text(arena_t *arena, const char *text, size_t max_bytes)
{
    if (!text) return NULL;
    bool cut = false;
    size_t length = bounded_length(text, max_bytes, &cut);
    size_t room = arena->size - arena->used;
    if (room == 0U) {
        arena_exhaust(arena);
        return NULL;
    }
    const bool short_of_room = length + 1U > room;
    if (short_of_room) {
        length = room - 1U;
        cut = true;
    }
    if (cut) length = ui_store_utf8_prefix(text, strnlen(text, max_bytes), length);
    char *out = (char *)arena->base + arena->used;
    memcpy(out, text, length);
    out[length] = '\0';
    arena->used += length + 1U;
    if (short_of_room) {
        arena_exhaust(arena);
    } else if (cut) {
        ++arena->cuts;
    }
    return out;
}

/* A copy of a name that selects something: whole or NULL. */
static const char *arena_name(arena_t *arena, const char *text)
{
    if (!text) return NULL;
    bool cut = false;
    size_t length = bounded_length(text, XIAOTAI_VIEW_TEXT_MAX, &cut);
    if (cut) {
        ++arena->cuts;
        return NULL;
    }
    if (length + 1U > arena->size - arena->used) {
        arena_exhaust(arena);
        return NULL;
    }
    char *out = (char *)arena->base + arena->used;
    memcpy(out, text, length + 1U);
    arena->used += length + 1U;
    return out;
}

/* ------------------------------------------------------------------- copy */

static void copy_scalars(xiaotai_view_t *out, const xiaotai_view_t *view, xiaotai_view_layer_t layer)
{
    out->kind = view->kind;
    out->flags = view->flags;
    out->duration_ms = view->duration_ms;
    out->layer = (uint8_t)layer;
    out->display = view->display;
    if (layer == XIAOTAI_LAYER_TOAST) return;
    out->timer_s = view->timer_s;
    out->focus = view->focus;
    out->timer_mode = view->timer_mode;
    out->phase = view->phase;
    out->level = view->level;
    out->speaker = view->speaker;
    out->progress = view->progress;
}

static void copy_actions(arena_t *arena, xiaotai_view_t *out, const xiaotai_view_t *view)
{
    size_t count = view->actions ? view->action_count : 0U;
    if (count > XIAOTAI_VIEW_ACTIONS_MAX) {
        count = XIAOTAI_VIEW_ACTIONS_MAX;
        ++arena->cuts;
    }
    xiaotai_view_action_t *actions =
        arena_array(arena, sizeof(*actions), alignof(xiaotai_view_action_t), &count);
    for (size_t i = 0; i < count; ++i) {
        actions[i] = view->actions[i];
        actions[i].intent = arena_name(arena, view->actions[i].intent);
        actions[i].icon = arena_name(arena, view->actions[i].icon);
        actions[i].label = arena_text(arena, view->actions[i].label, XIAOTAI_VIEW_TEXT_MAX);
    }
    out->actions = actions;
    out->action_count = count;
}

static void copy_options(arena_t *arena, xiaotai_view_field_t *field, const xiaotai_view_field_t *source)
{
    size_t count = source->options ? source->option_count : 0U;
    if (count > XIAOTAI_VIEW_OPTIONS_MAX) {
        count = XIAOTAI_VIEW_OPTIONS_MAX;
        ++arena->cuts;
    }
    const char **options = arena_array(arena, sizeof(*options), alignof(const char *), &count);
    for (size_t i = 0; i < count; ++i) {
        options[i] = arena_text(arena, source->options[i], XIAOTAI_VIEW_TEXT_MAX);
    }
    field->options = options;
    field->option_count = count;
}

static void copy_fields(arena_t *arena, xiaotai_view_t *out, const xiaotai_view_t *view)
{
    size_t count = view->fields ? view->field_count : 0U;
    if (count > XIAOTAI_VIEW_FIELDS_MAX) {
        count = XIAOTAI_VIEW_FIELDS_MAX;
        ++arena->cuts;
    }
    xiaotai_view_field_t *fields = arena_array(arena, sizeof(*fields), alignof(xiaotai_view_field_t), &count);
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_view_field_t *source = &view->fields[i];
        fields[i] = *source;
        fields[i].options = NULL;
        fields[i].option_count = 0U;
        fields[i].intent = arena_name(arena, source->intent);
        fields[i].label = arena_text(arena, source->label, XIAOTAI_VIEW_TEXT_MAX);
        if (source->kind == XIAOTAI_FIELD_DIGITS) {
            /* A DIGITS field has at most XIAOTAI_VIEW_DIGITS_MAX digits. */
            if (fields[i].max > (int32_t)XIAOTAI_VIEW_DIGITS_MAX) {
                fields[i].max = (int32_t)XIAOTAI_VIEW_DIGITS_MAX;
                ++arena->cuts;
            }
            fields[i].text = arena_text(arena, source->text, XIAOTAI_VIEW_DIGITS_MAX + 1U);
        } else {
            fields[i].text = arena_text(arena, source->text, XIAOTAI_VIEW_TEXT_MAX);
        }
        if (source->kind == XIAOTAI_FIELD_CHOICE) copy_options(arena, &fields[i], source);
    }
    out->fields = fields;
    out->field_count = count;
}

static void copy_indicators(arena_t *arena, xiaotai_view_t *out, const xiaotai_view_t *view)
{
    size_t count = view->indicators ? view->indicator_count : 0U;
    if (count > XIAOTAI_VIEW_INDICATORS_MAX) {
        count = XIAOTAI_VIEW_INDICATORS_MAX;
        ++arena->cuts;
    }
    xiaotai_view_indicator_t *indicators =
        arena_array(arena, sizeof(*indicators), alignof(xiaotai_view_indicator_t), &count);
    for (size_t i = 0; i < count; ++i) {
        indicators[i] = view->indicators[i];
        indicators[i].icon = arena_name(arena, view->indicators[i].icon);
        indicators[i].intent = arena_name(arena, view->indicators[i].intent);
    }
    out->indicators = indicators;
    out->indicator_count = count;
}

/* Items in two passes: first every label (with what makes the item operable:
 * intent and icon), then the details, so a long list loses details before it
 * loses entries. */
static void copy_items(arena_t *arena, xiaotai_view_t *out, const xiaotai_view_t *view)
{
    size_t count = view->items ? view->item_count : 0U;
    if (count > XIAOTAI_VIEW_ITEMS_MAX) {
        count = XIAOTAI_VIEW_ITEMS_MAX;
        ++arena->cuts;
    }
    xiaotai_view_item_t *items = arena_array(arena, sizeof(*items), alignof(xiaotai_view_item_t), &count);
    for (size_t i = 0; i < count; ++i) {
        items[i] = view->items[i];
        items[i].detail = NULL;
        items[i].intent = arena_name(arena, view->items[i].intent);
        items[i].icon = arena_name(arena, view->items[i].icon);
        items[i].label = arena_text(arena, view->items[i].label, XIAOTAI_VIEW_TEXT_MAX);
    }
    for (size_t i = 0; i < count; ++i) {
        items[i].detail = arena_text(arena, view->items[i].detail, XIAOTAI_VIEW_TEXT_MAX);
    }
    out->items = items;
    out->item_count = count;
}

static void copy_custom_data(arena_t *arena, xiaotai_view_t *out, const xiaotai_view_t *view)
{
    size_t size = view->custom_data ? view->custom_size : 0U;
    if (size > XIAOTAI_VIEW_CUSTOM_MAX) {
        size = XIAOTAI_VIEW_CUSTOM_MAX;
        ++arena->cuts;
    }
    if (size == 0U) return;
    /* Templates read structures from it: all of it or nothing. */
    if (arena_free(arena, alignof(max_align_t)) < size) {
        arena_exhaust(arena);
        return;
    }
    size_t count = size;
    uint8_t *data = arena_array(arena, 1U, alignof(max_align_t), &count);
    memcpy(data, view->custom_data, size);
    out->custom_data = data;
    out->custom_size = size;
}

uint32_t ui_store_copy(ui_copy_t *copy, size_t capacity, const xiaotai_view_t *view, xiaotai_view_layer_t layer)
{
    arena_t arena = {.base = copy->arena, .size = copy->arena ? capacity : 0U};
    xiaotai_view_t *out = &copy->view;
    memset(out, 0, sizeof(*out));
    copy_scalars(out, view, layer);
    out->custom = arena_name(&arena, view->custom);
    out->id = arena_name(&arena, view->id);
    if (layer == XIAOTAI_LAYER_TOAST) {
        /* A toast is text only and takes no input. */
        out->text = arena_text(&arena, view->text, XIAOTAI_VIEW_TEXT_MAX);
        copy->present = true;
        return arena.cuts;
    }
    out->title = arena_text(&arena, view->title, XIAOTAI_VIEW_TEXT_MAX);
    out->subtitle = arena_text(&arena, view->subtitle, XIAOTAI_VIEW_TEXT_MAX);
    out->status = arena_text(&arena, view->status, XIAOTAI_VIEW_TEXT_MAX);
    out->emotion = arena_name(&arena, view->emotion);
    out->image = arena_name(&arena, view->image);
    copy_custom_data(&arena, out, view);
    copy_actions(&arena, out, view);
    copy_fields(&arena, out, view);
    copy_indicators(&arena, out, view);
    copy_items(&arena, out, view);
    out->text = arena_text(&arena, view->text, XIAOTAI_VIEW_TEXT_MAX);
    copy->present = true;
    return arena.cuts;
}

/* ---------------------------------------------------------------- compare */

static bool same_text(const char *a, const char *b)
{
    if (a == b) return true;
    if (!a || !b) return false;
    return strcmp(a, b) == 0;
}

bool ui_store_same_identity(const xiaotai_view_t *source, size_t capacity, xiaotai_view_layer_t layer,
                            const xiaotai_view_t *copy)
{
    (void)layer;
    if (source->kind != copy->kind) return false;
    /* custom and id are the first two names of every copy, kept whole or
     * dropped by the room left, exactly as ui_store_copy() decides. */
    size_t room = capacity;
    const char *names[2] = {source->custom, source->id};
    const char *kept[2] = {copy->custom, copy->id};
    for (size_t i = 0; i < 2U; ++i) {
        const char *name = names[i];
        if (name) {
            bool cut = false;
            size_t length = bounded_length(name, XIAOTAI_VIEW_TEXT_MAX, &cut);
            if (cut) {
                name = NULL;
            } else if (length + 1U > room) {
                name = NULL;
                room = 0U;   /* the arena is exhausted from here on */
            } else {
                room -= length + 1U;
            }
        }
        if (!same_text(name, kept[i])) return false;
    }
    return true;
}

static bool same_items(const xiaotai_view_t *a, const xiaotai_view_t *b)
{
    if (a->item_count != b->item_count) return false;
    for (size_t i = 0; i < a->item_count; ++i) {
        const xiaotai_view_item_t *x = &a->items[i], *y = &b->items[i];
        if (x->value != y->value || x->state != y->state || !same_text(x->label, y->label) ||
            !same_text(x->detail, y->detail) || !same_text(x->intent, y->intent) || !same_text(x->icon, y->icon)) {
            return false;
        }
    }
    return true;
}

static bool same_actions(const xiaotai_view_t *a, const xiaotai_view_t *b)
{
    if (a->action_count != b->action_count) return false;
    for (size_t i = 0; i < a->action_count; ++i) {
        const xiaotai_view_action_t *x = &a->actions[i], *y = &b->actions[i];
        if (x->value != y->value || x->trigger != y->trigger || x->role != y->role || x->state != y->state ||
            x->style != y->style || !same_text(x->label, y->label) || !same_text(x->intent, y->intent) ||
            !same_text(x->icon, y->icon)) {
            return false;
        }
    }
    return true;
}

static bool same_fields(const xiaotai_view_t *a, const xiaotai_view_t *b)
{
    if (a->field_count != b->field_count) return false;
    for (size_t i = 0; i < a->field_count; ++i) {
        const xiaotai_view_field_t *x = &a->fields[i], *y = &b->fields[i];
        if (x->kind != y->kind || x->state != y->state || x->flags != y->flags || x->value != y->value ||
            x->min != y->min || x->max != y->max || x->step != y->step || !same_text(x->label, y->label) ||
            !same_text(x->intent, y->intent) || !same_text(x->text, y->text) ||
            x->option_count != y->option_count) {
            return false;
        }
        for (size_t o = 0; o < x->option_count; ++o) {
            if (!same_text(x->options[o], y->options[o])) return false;
        }
    }
    return true;
}

static bool same_indicators(const xiaotai_view_t *a, const xiaotai_view_t *b)
{
    if (a->indicator_count != b->indicator_count) return false;
    for (size_t i = 0; i < a->indicator_count; ++i) {
        const xiaotai_view_indicator_t *x = &a->indicators[i], *y = &b->indicators[i];
        if (x->level != y->level || x->value != y->value || !same_text(x->icon, y->icon) ||
            !same_text(x->intent, y->intent)) {
            return false;
        }
    }
    return true;
}

bool ui_store_same_controls(const xiaotai_view_t *before, const xiaotai_view_t *after)
{
    if ((after->action_count && !after->actions) || (after->item_count && !after->items) ||
        (after->field_count && !after->fields) || (after->indicator_count && !after->indicators) ||
        before->action_count != after->action_count || before->item_count != after->item_count ||
        before->field_count != after->field_count || before->indicator_count != after->indicator_count) return false;
    for (size_t i = 0; i < after->action_count; ++i) {
        const xiaotai_view_action_t *a = &before->actions[i], *b = &after->actions[i];
        if (a->value != b->value || a->role != b->role || a->trigger != b->trigger || a->state != b->state ||
            !same_text(a->intent, b->intent)) return false;
    }
    for (size_t i = 0; i < after->item_count; ++i) {
        const xiaotai_view_item_t *a = &before->items[i], *b = &after->items[i];
        if (a->value != b->value || a->state != b->state || !same_text(a->intent, b->intent)) return false;
    }
    for (size_t i = 0; i < after->field_count; ++i) {
        const xiaotai_view_field_t *a = &before->fields[i], *b = &after->fields[i];
        if (!same_text(a->intent, b->intent) || a->kind != b->kind || a->state != b->state || a->flags != b->flags ||
            a->min != b->min || a->max != b->max || a->step != b->step || a->option_count != b->option_count)
            return false;
    }
    for (size_t i = 0; i < after->indicator_count; ++i) {
        const xiaotai_view_indicator_t *a = &before->indicators[i], *b = &after->indicators[i];
        if (a->value != b->value || !same_text(a->intent, b->intent)) return false;
    }
    return true;
}

uint32_t ui_store_diff(const xiaotai_view_t *before, const xiaotai_view_t *after)
{
    uint32_t changed = 0U;
    if (!same_text(before->title, after->title) || !same_text(before->subtitle, after->subtitle) ||
        !same_text(before->status, after->status)) {
        changed |= XIAOTAI_CHANGED_TITLE;
    }
    if (!same_text(before->text, after->text) || before->speaker != after->speaker) {
        changed |= XIAOTAI_CHANGED_TEXT;
    }
    if (!same_text(before->emotion, after->emotion) || before->phase != after->phase ||
        before->level != after->level) {
        changed |= XIAOTAI_CHANGED_EMOTION;
    }
    if (!same_items(before, after)) changed |= XIAOTAI_CHANGED_ITEMS;
    if (!same_actions(before, after)) changed |= XIAOTAI_CHANGED_ACTIONS;
    if (!same_fields(before, after)) changed |= XIAOTAI_CHANGED_FIELDS;
    if (!same_indicators(before, after)) changed |= XIAOTAI_CHANGED_INDICATORS;
    if (before->timer_s != after->timer_s || before->timer_mode != after->timer_mode) {
        changed |= XIAOTAI_CHANGED_TIMER;
    }
    if (before->progress != after->progress) changed |= XIAOTAI_CHANGED_PROGRESS;
    if (before->focus != after->focus) changed |= XIAOTAI_CHANGED_FOCUS;
    if (!same_text(before->image, after->image)) changed |= XIAOTAI_CHANGED_IMAGE;
    if (before->custom_size != after->custom_size ||
        (before->custom_size && memcmp(before->custom_data, after->custom_data, before->custom_size) != 0)) {
        changed |= XIAOTAI_CHANGED_CUSTOM;
    }
    if (before->flags != after->flags) changed |= XIAOTAI_CHANGED_FLAGS;
    return changed;
}
