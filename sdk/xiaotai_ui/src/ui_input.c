// SPDX-License-Identifier: MIT
#include "ui_internal.h"

#include <string.h>
#include "xiaotai/port/mem.h"

#define SWIPE_INTERVAL_MS 350U
#define INTERACTION_INTERVAL_MS 250U
#define RELEASE_RETRY_MS 10U
#define REJECT_RETRY_MS 20U
#define REJECT_LIMIT_MS 1000U
#define INERT_STATES (XIAOTAI_STATE_DISABLED | XIAOTAI_STATE_BUSY | XIAOTAI_STATE_HIDDEN)

static uint32_t rejected_serial;  /* protected by g_ui.lock; not reset with the display */

static bool text_present(const char *text) { return text && text[0]; }
static bool available(uint8_t state) { return (state & INERT_STATES) == 0U; }

static void emit(const char *event, int32_t value, const char *text)
{
    xiaotai_args_t args = {.value = value};
    if (text) memcpy(args.text, text, ui_store_utf8_prefix(text, strlen(text), sizeof(args.text) - 1U));
    (void)xiaotai_event_emit(event, &args, NULL, 0U);
}

static void rejected(ui_display_t *display, const char *intent)
{
    if (!text_present(intent)) return;
    xiaotai_args_t args = {0};
    memcpy(args.text, intent, ui_store_utf8_prefix(intent, strlen(intent), sizeof(args.text) - 1U));
    if (xiaotai_event_emit(XIAOTAI_EVENT_UI_REJECTED, &args, NULL, 0U) != XIAOTAI_ERR_NO_MEM) return;

    const uint32_t now = ui_now_ms();
    xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER);
    if (display->state == UI_DISPLAY_READY && display->rendered_rejection_scope == display->rejection_scope) {
        if (!display->rejected.pending || display->rejected.scope != display->rejection_scope ||
            (int32_t)(now - display->rejected.expiry) >= 0) {
            display->rejected.due = now + REJECT_RETRY_MS;
            display->rejected.expiry = now + REJECT_LIMIT_MS;
        }
        if (++rejected_serial == 0U) ++rejected_serial;
        display->rejected.ticket = rejected_serial;
        display->rejected.scope = display->rejection_scope;
        memcpy(display->rejected.intent, args.text, sizeof(args.text));
        display->rejected.pending = true;
    }
    xiaotai_mutex_unlock(&g_ui.lock);
}

static void publish_rejected(ui_display_t *display, uint32_t now)
{
    xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER);
    if (display->rejected.pending && (int32_t)(now - display->rejected.due) >= 0) {
        if (display->rejected.scope != display->rejection_scope ||
            (int32_t)(now - display->rejected.expiry) >= 0) {
            display->rejected.pending = false;
            display->rejected.ticket = 0U;
        } else {
            xiaotai_args_t args;
            memcpy(&args.value, &display->rejected.ticket, sizeof(args.value));
            memcpy(args.text, display->rejected.intent, sizeof(args.text));
            const xiaotai_err_t err = xiaotai_event_emit(XIAOTAI_EVENT_UI_REJECTED, &args, NULL, 0U);
            display->rejected.pending = err == XIAOTAI_ERR_NO_MEM;
            if (display->rejected.pending) display->rejected.due = now + REJECT_RETRY_MS;
            else if (err != XIAOTAI_OK) display->rejected.ticket = 0U;
        }
    }
    xiaotai_mutex_unlock(&g_ui.lock);
}

bool xiaotai_ui_rejection_current(const xiaotai_args_t *args)
{
    if (!args || !g_ui.initialized || !xiaotai_on_owner()) return false;
    if (args->value == 0) return true;
    uint32_t ticket;
    memcpy(&ticket, &args->value, sizeof(ticket));
    bool current = false;
    const uint32_t now = ui_now_ms();
    xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER);
    for (size_t i = 0; i < g_ui.display_count; ++i) {
        const ui_display_t *d = &g_ui.displays[i];
        if (d->state == UI_DISPLAY_READY && d->rejected.ticket == ticket && !d->rejected.pending &&
            d->rejected.scope == d->rejection_scope &&
            (int32_t)(now - d->rejected.expiry) < 0 &&
            !strncmp(args->text, d->rejected.intent, sizeof(args->text))) {
            current = true;
            break;
        }
    }
    xiaotai_mutex_unlock(&g_ui.lock);
    return current;
}

void ui_input_ignored(ui_display_t *display)
{
    (void)xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER);
    ++display->counters.ignored;
    xiaotai_mutex_unlock(&g_ui.lock);
}

static xiaotai_err_t post(ui_display_t *display, const char *intent, int32_t value, const char *text)
{
    xiaotai_args_t args = {.value = value};
    if (!text_present(intent)) return XIAOTAI_ERR_NOT_FOUND;
    if (text) memcpy(args.text, text, ui_store_utf8_prefix(text, strlen(text), sizeof(args.text) - 1U));
    const xiaotai_err_t result = xiaotai_intent_post(intent, &args);
    (void)xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER);
    if (result == XIAOTAI_OK) {
        ++display->counters.posted;
        if (display->rejected.ticket && !strcmp(intent, display->rejected.intent)) {
            display->rejected.ticket = 0U;
            display->rejected.pending = false;
        }
    } else ++display->counters.rejected;
    xiaotai_mutex_unlock(&g_ui.lock);
    return result;
}

static bool send(ui_display_t *display, const char *intent, int32_t value, const char *text)
{
    if (post(display, intent, value, text) == XIAOTAI_OK) return true;
    ui_input_ignored(display);
    rejected(display, intent);
    return false;
}

static void activity(ui_display_t *display, uint32_t now)
{
    if (display->activity_seen && (uint32_t)(now - display->activity_at) < INTERACTION_INTERVAL_MS) return;
    display->activity_at = now;
    display->activity_seen = true;
    emit(XIAOTAI_EVENT_UI_INTERACTION, display->host.index, NULL);
}

static const char *hold_name(const ui_press_t *press)
{
    if (press->hold_ticket) return g_ui.local_hold.intent;
    return press->hold_name_long ? press->hold_name_long : press->hold_intent;
}

xiaotai_err_t xiaotai_ui_local_hold_register(const char *intent, xiaotai_ui_local_hold_fn_t fn, void *ctx)
{
    if (!text_present(intent) || !fn) return XIAOTAI_ERR_INVALID_ARG;
    if (!g_ui.initialized || g_ui.started || g_ui.local_hold.fn) return XIAOTAI_ERR_INVALID_STATE;
    if (g_ui.display_count == 0U) return XIAOTAI_ERR_NOT_SUPPORTED;
    g_ui.local_hold.intent = intent;
    g_ui.local_hold.fn = fn;
    g_ui.local_hold.ctx = ctx;
    return XIAOTAI_OK;
}

void ui_input_end_local_hold(ui_display_t *display)
{
    ui_press_t *press = &display->press;
    const uint32_t ticket = press->hold_ticket;
    if (ticket == 0U) return;
    press->hold_ticket = 0U;
    press->hold_from_pointer = false;
    if (g_ui.local_hold.fn(g_ui.local_hold.ctx, ticket, press->hold_value, XIAOTAI_UI_HOLD_END) != XIAOTAI_OK)
        ui_input_ignored(display);
}

void ui_input_reset(ui_display_t *display)
{
    xiaotai_mem_free(display->press.hold_name_long);
    display->press.hold_name_long = NULL;
    display->press.hold_intent[0] = '\0';
    display->press.release_pending = false;
}

static void release_hold(ui_display_t *display, uint32_t now)
{
    ui_press_t *press = &display->press;
    if (press->hold_ticket) {
        ui_input_end_local_hold(display);
        return;
    }
    const char *intent = hold_name(press);
    if (!*intent) return;
    const xiaotai_err_t result = post(display, intent, press->hold_value, XIAOTAI_ARG_RELEASE);
    if (result == XIAOTAI_ERR_NO_MEM) {
        /* Retain the accepted press until its release can enter the owner
         * queue. Further presses cannot overwrite this obligation. */
        press->release_pending = true;
        press->hold_due_ms = now + RELEASE_RETRY_MS;
        return;
    }
    if (result != XIAOTAI_OK) {
        ui_input_ignored(display);
        rejected(display, intent);
    }
    ui_input_reset(display);
}

static void begin_hold(ui_display_t *display, const xiaotai_view_action_t *action, uint32_t now)
{
    ui_press_t *press = &display->press;
    if (press->release_pending || !text_present(action->intent)) {
        ui_input_ignored(display);
        return;
    }
    if (display->host.index == 0U && g_ui.local_hold.fn && !strcmp(action->intent, g_ui.local_hold.intent)) {
        /* Only display 0's UI task issues tickets. Keep the serial across UI
         * reset so a replacement press cannot share the retired ticket. */
        static uint32_t serial;
        if (++serial == 0U) ++serial;
        if (g_ui.local_hold.fn(g_ui.local_hold.ctx, serial, action->value, XIAOTAI_UI_HOLD_BEGIN) == XIAOTAI_OK) {
            press->hold_ticket = serial;
            press->hold_value = action->value;
            press->hold_from_pointer = (action->state & XIAOTAI_STATE_HOLD_FROM_POINTER) != 0;
        } else {
            ui_input_ignored(display);
        }
        return;
    }
    if (action->state & XIAOTAI_STATE_HOLD_FROM_POINTER) {
        /* A pointer-verified action must not fall back to a timer-generated hold. */
        ui_input_ignored(display);
        return;
    }
    const size_t bytes = strlen(action->intent) + 1U;
    if (bytes > sizeof(press->hold_intent)) {
        press->hold_name_long = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, bytes);
        if (!press->hold_name_long) {
            ui_input_ignored(display);
            rejected(display, action->intent);
            return;
        }
    }
    memcpy(press->hold_name_long ? press->hold_name_long : press->hold_intent, action->intent, bytes);
    press->hold_value = action->value;
    press->hold_due_ms = now + XIAOTAI_VIEW_HOLD_REPEAT_MS;
    if (!send(display, action->intent, action->value, XIAOTAI_ARG_PRESS)) ui_input_reset(display);
}

static void suppress(ui_display_t *display, uint32_t now)
{
    display->press.suppressed = true;
    if (!display->press.release_pending) release_hold(display, now);
}

void ui_input_cancel_press(ui_display_t *display)
{
    if (display->press.down) suppress(display, ui_now_ms());
    display->press.down = false;
}

static bool same_binding(const char *a, const char *b)
{
    return strcmp(a ? a : "", b ? b : "") == 0;
}

bool ui_input_target_changed(const ui_press_t *press, const xiaotai_view_t *old, const xiaotai_view_t *next)
{
    const size_t i = press->index;
    switch (press->element) {
        case XIAOTAI_UI_ELEMENT_ITEM:
            return i >= old->item_count || i >= next->item_count ||
                   old->items[i].state != next->items[i].state || old->items[i].value != next->items[i].value ||
                   !same_binding(old->items[i].intent, next->items[i].intent);
        case XIAOTAI_UI_ELEMENT_ACTION:
            return i >= old->action_count || i >= next->action_count ||
                   old->actions[i].state != next->actions[i].state ||
                   old->actions[i].value != next->actions[i].value ||
                   old->actions[i].trigger != next->actions[i].trigger ||
                   !same_binding(old->actions[i].intent, next->actions[i].intent);
        case XIAOTAI_UI_ELEMENT_FIELD:
            return i >= old->field_count || i >= next->field_count ||
                   old->fields[i].state != next->fields[i].state || old->fields[i].kind != next->fields[i].kind ||
                   old->fields[i].min != next->fields[i].min || old->fields[i].max != next->fields[i].max ||
                   !same_binding(old->fields[i].intent, next->fields[i].intent);
        case XIAOTAI_UI_ELEMENT_INDICATOR:
            return i >= old->indicator_count || i >= next->indicator_count ||
                   old->indicators[i].value != next->indicators[i].value ||
                   !same_binding(old->indicators[i].intent, next->indicators[i].intent);
        default:
            return false;
    }
}

void ui_input_scene_changed(ui_display_t *display, const ui_take_t take[UI_LAYER_COUNT])
{
    const ui_press_t *press = &display->press;
    if (!press->down || press->suppressed) return;
    const xiaotai_ui_layer_t *layer = ui_scene_layer(display, press->layer);
    if (press->layer != ui_scene_top(display) || !layer->present ||
        (layer->changed & XIAOTAI_CHANGED_IDENTITY)) {
        suppress(display, display->scene.now_ms);
        return;
    }
    if (*hold_name(press)) {
        const xiaotai_view_action_t *action =
            press->index < layer->view.action_count ? &layer->view.actions[press->index] : NULL;
        if (!action || !available(action->state) || action->trigger != XIAOTAI_TRIGGER_HOLD ||
            (press->hold_ticket && press->hold_from_pointer !=
                                  ((action->state & XIAOTAI_STATE_HOLD_FROM_POINTER) != 0)) ||
            action->value != press->hold_value || !action->intent || strcmp(action->intent, hold_name(press))) {
            suppress(display, display->scene.now_ms);
        }
    } else if (take[press->layer].press_changed) {
        suppress(display, display->scene.now_ms);
    }
}

static void role_action(ui_display_t *display, const xiaotai_view_t *view, uint8_t role)
{
    for (size_t i = 0; i < view->action_count; ++i) {
        const xiaotai_view_action_t *action = &view->actions[i];
        if (action->role != role) continue;
        if (available(action->state)) (void)send(display, action->intent, action->value, NULL);
        else ui_input_ignored(display);
        return;
    }
}

static size_t digit_count(const xiaotai_view_field_t *field)
{
    return field->max > 0 && (uint32_t)field->max <= XIAOTAI_VIEW_DIGITS_MAX ? (size_t)field->max : 0U;
}

static bool valid_digits(const char *text, size_t count)
{
    return text && count && strnlen(text, count + 1U) == count && strspn(text, "0123456789") == count;
}

static bool post_field(ui_display_t *display, const xiaotai_view_field_t *field, bool live,
                       int32_t value, const char *text)
{
    if (!available(field->state)) goto ignored;
    if (live && (field->kind != XIAOTAI_FIELD_SLIDER || !(field->flags & XIAOTAI_FIELD_LIVE))) return false;
    switch (field->kind) {
        case XIAOTAI_FIELD_SWITCH:
            if (value != 0 && value != 1) goto ignored;
            break;
        case XIAOTAI_FIELD_STEPPER:
        case XIAOTAI_FIELD_SLIDER:
            if (value < field->min || value > field->max) goto ignored;
            break;
        case XIAOTAI_FIELD_CHOICE:
            if (value < 0 || (size_t)value >= field->option_count) goto ignored;
            break;
        case XIAOTAI_FIELD_DIGITS: {
            const size_t count = digit_count(field);
            const bool empty = count && (field->flags & XIAOTAI_FIELD_ALLOW_EMPTY) && text && !*text;
            if (!empty && !valid_digits(text, count)) goto ignored;
            return send(display, field->intent, 0, text);
        }
        default: goto ignored;
    }
    return send(display, field->intent, value, live ? XIAOTAI_ARG_LIVE : NULL);
ignored:
    ui_input_ignored(display);
    return false;
}

static void edit_digit(ui_display_t *display, xiaotai_view_layer_t layer, xiaotai_ui_layer_t *out,
                       const xiaotai_view_field_t *field, ui_nav_t nav)
{
    const size_t count = digit_count(field);
    if (!count || out->cursor >= count) {
        ui_input_ignored(display);
        return;
    }
    if (nav == UI_NAV_CONFIRM) {
        if (out->cursor + 1U < count) {
            ++out->cursor;
            display->scene_pending[layer] |= XIAOTAI_CHANGED_FIELDS;
            return;
        }
        for (size_t i = out->focus + 1U; i < out->view.field_count; ++i) {
            if (out->view.fields[i].kind == XIAOTAI_FIELD_DIGITS) {
                ui_scene_focus(display, layer, XIAOTAI_UI_FOCUS_FIELD, (uint16_t)i);
                return;
            }
        }
        role_action(display, &out->view, XIAOTAI_ROLE_PRIMARY);
        return;
    }
    char *digits = display->scene_layer[layer].digits;
    if (!valid_digits(digits, count)) {
        if (valid_digits(field->text, count)) memcpy(digits, field->text, count);
        else memset(digits, '0', count);
        digits[count] = '\0';
    }
    const char old = digits[out->cursor];
    digits[out->cursor] = (char)('0' + (old - '0' + (nav == UI_NAV_NEXT ? 1 : 9)) % 10);
    if (!post_field(display, field, false, 0, digits)) digits[out->cursor] = old;
}

static void navigate(ui_display_t *display, ui_nav_t nav, uint32_t now)
{
    activity(display, now);
    if (!ui_power_navigation(display, now)) return;
    const xiaotai_view_layer_t top = ui_scene_top(display);
    xiaotai_ui_layer_t *out = ui_scene_layer(display, top);
    if (!out->present) return;
    const xiaotai_view_t *view = &out->view;
    if (nav == UI_NAV_BACK) {
        role_action(display, view, XIAOTAI_ROLE_BACK);
        return;
    }
    const bool items = out->focus_kind == XIAOTAI_UI_FOCUS_ITEM;
    const bool fields = out->focus_kind == XIAOTAI_UI_FOCUS_FIELD;
    const size_t count = items ? view->item_count : fields ? view->field_count : 0U;
    if (!count || out->focus >= count) {
        role_action(display, view, nav == UI_NAV_CONFIRM ? XIAOTAI_ROLE_PRIMARY : XIAOTAI_ROLE_AUX);
        return;
    }
    const xiaotai_view_field_t *field = fields ? &view->fields[out->focus] : NULL;
    if (field && field->kind == XIAOTAI_FIELD_DIGITS) {
        if (available(field->state)) edit_digit(display, top, out, field, nav);
        else ui_input_ignored(display);
    } else if (nav != UI_NAV_CONFIRM) {
        const size_t offset = nav == UI_NAV_NEXT ? 1U : count - 1U;
        ui_scene_focus(display, top, out->focus_kind, (uint16_t)((out->focus + offset) % count));
    } else if (items) {
        const xiaotai_view_item_t *item = &view->items[out->focus];
        if (available(item->state)) (void)send(display, item->intent, item->value, NULL);
        else ui_input_ignored(display);
    } else if (!available(field->state)) {
        ui_input_ignored(display);
    } else if (field->kind == XIAOTAI_FIELD_SWITCH) {
        (void)post_field(display, field, false, !field->value, NULL);
    } else if (field->kind == XIAOTAI_FIELD_CHOICE) {
        const int32_t next = field->value >= 0 && (size_t)field->value + 1U < field->option_count ? field->value + 1 : 0;
        (void)post_field(display, field, false, next, NULL);
    } else if (field->kind == XIAOTAI_FIELD_STEPPER) {
        const int64_t next = (int64_t)field->value + (field->step > 0 ? field->step : 1);
        (void)post_field(display, field, false, next > field->max ? field->min : (int32_t)next, NULL);
    }
}

static void queue_nav(const xiaotai_args_t *args, void *ctx)
{
    const ui_nav_t nav = *(const ui_nav_t *)ctx;
    ui_display_t *display = args->value >= 0 ? ui_display_at((size_t)args->value) : NULL;
    if (!display) {
        display = ui_display_at(0U);
        if (display) ui_input_ignored(display);
        return;
    }
    (void)xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER);
    if (display->nav_count == UI_NAV_DEPTH || display->state == UI_DISPLAY_CLOSED) {
        ++display->counters.ignored;
    } else {
        const size_t tail = (display->nav_head + display->nav_count) % UI_NAV_DEPTH;
        display->nav_queue[tail] = nav;
        display->nav_epoch[tail] = display->input_epoch;
        ++display->nav_count;
    }
    xiaotai_mutex_unlock(&g_ui.lock);
    (void)ui_driver_kick(display);
}

xiaotai_err_t ui_input_register(void)
{
    static ui_nav_t controls[] = {UI_NAV_NEXT, UI_NAV_PREVIOUS, UI_NAV_CONFIRM, UI_NAV_BACK};
    const char *const names[] = {XIAOTAI_INTENT_UI_NEXT, XIAOTAI_INTENT_UI_PREVIOUS,
                                 XIAOTAI_INTENT_UI_CONFIRM, XIAOTAI_INTENT_UI_BACK};
    for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); ++n) {
        /* Physical navigation may reach local-only actions. An external
         * intent cannot use a navigation key to bypass their local flag. */
        const xiaotai_err_t result = xiaotai_intent_handle_ex(names[n], queue_nav, &controls[n],
                                                             XIAOTAI_INTENT_LOCAL);
        if (result != XIAOTAI_OK) return result;
    }
    return XIAOTAI_OK;
}

void ui_input_step(ui_display_t *display, uint32_t now)
{
    publish_rejected(display, now);
    uint8_t pending[UI_NAV_DEPTH], count = 0U;
    (void)xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER);
    while (display->nav_count) {
        const uint8_t head = display->nav_head;
        if (display->nav_epoch[head] == display->input_epoch) pending[count++] = display->nav_queue[head];
        else ++display->counters.ignored;
        display->nav_head = (head + 1U) % UI_NAV_DEPTH;
        --display->nav_count;
    }
    xiaotai_mutex_unlock(&g_ui.lock);
    for (uint8_t i = 0; i < count; ++i) navigate(display, pending[i], now);
    ui_press_t *press = &display->press;
    if (press->hold_ticket || !*hold_name(press) || (int32_t)(now - press->hold_due_ms) < 0) return;
    if (press->release_pending) release_hold(display, now);
    else {
        (void)send(display, hold_name(press), press->hold_value, XIAOTAI_ARG_PRESS);
        press->hold_due_ms = now + XIAOTAI_VIEW_HOLD_REPEAT_MS;
    }
}

static void swipe(ui_display_t *display, xiaotai_view_layer_t layer, xiaotai_ui_input_t input, uint32_t now)
{
    ui_press_t *press = &display->press;
    if (press->swiped || press->suppressed) {
        ui_input_ignored(display);
        return;
    }
    press->swiped = true;
    suppress(display, now);
    for (size_t n = 0; n < 2U; ++n) {
        if (display->swipe_seen[n] && (uint32_t)(now - display->swipe_at[n]) < SWIPE_INTERVAL_MS) {
            ui_input_ignored(display);
            return;
        }
    }
    display->swipe_seen[layer] = true;
    display->swipe_at[layer] = now;
    const xiaotai_view_t *view = &ui_scene_layer(display, layer)->view;
    const uint8_t trigger = XIAOTAI_TRIGGER_SWIPE_LEFT + input - XIAOTAI_UI_INPUT_SWIPE_LEFT;
    for (size_t n = 0; n < view->action_count; ++n) {
        const xiaotai_view_action_t *action = &view->actions[n];
        if (action->trigger != trigger) continue;
        if (available(action->state)) (void)send(display, action->intent, action->value, NULL);
        else ui_input_ignored(display);
        return;
    }
}

static void tap_action(ui_display_t *display, const xiaotai_view_action_t *action, uint32_t now)
{
    const xiaotai_err_t err = post(display, action->intent, action->value, NULL);
    if (err == XIAOTAI_OK) return;
    ui_input_ignored(display);
    const char *text = g_ui_config.local_rejection_toast;
    if (err == XIAOTAI_ERR_NO_MEM && (action->state & XIAOTAI_STATE_TAP_LOCAL_REJECTION) && text_present(text))
        ui_local_rejection_toast(display, text, now);
    else
        rejected(display, action->intent);
}

static void action_input(ui_display_t *display, xiaotai_view_layer_t layer, const xiaotai_view_t *view,
                         const xiaotai_view_action_t *action, xiaotai_ui_input_t input, uint32_t now)
{
    if (!available(action->state)) {
        ui_input_ignored(display);
        return;
    }
    if (input == XIAOTAI_UI_INPUT_PRESS && action->trigger == XIAOTAI_TRIGGER_HOLD) {
        begin_hold(display, action, now);
    } else if (input == XIAOTAI_UI_INPUT_LONG_PRESS && action->trigger == XIAOTAI_TRIGGER_LONG_PRESS) {
        if (display->press.long_fired) return;
        display->press.long_fired = true;
        (void)send(display, action->intent, action->value, NULL);
    } else if (input == XIAOTAI_UI_INPUT_TAP) {
        const bool visible = text_present(action->label) || text_present(action->icon);
        const bool emotion = !visible && action->role == XIAOTAI_ROLE_PRIMARY &&
                             (view->kind == XIAOTAI_VIEW_HOME || view->kind == XIAOTAI_VIEW_TALK);
        if (emotion && display->swipe_seen[layer] &&
            (uint32_t)(now - display->swipe_at[layer]) < g_ui_config.swipe_tap_guard_ms) {
            ui_input_ignored(display);
            return;
        }
        if (action->trigger == XIAOTAI_TRIGGER_TAP ||
            (visible && action->trigger >= XIAOTAI_TRIGGER_SWIPE_LEFT &&
             action->trigger <= XIAOTAI_TRIGGER_SWIPE_DOWN)) {
            tap_action(display, action, now);
        }
    }
}

void xiaotai_ui_host_input(const xiaotai_ui_host_t *host, xiaotai_view_layer_t layer,
                           xiaotai_ui_element_t element, uint16_t index, xiaotai_ui_input_t input,
                           int32_t value, const char *text)
{
    ui_display_t *display = host ? ui_display_at(host->index) : NULL;
    if (!display || host != &display->host) return;
    const uint32_t now = ui_now_ms();
    ui_press_t *press = &display->press;
    const bool same = press->layer == layer && press->element == element && press->index == index;
    if (input == XIAOTAI_UI_INPUT_HOLD_REFRESH) {
        if (same && press->down && !press->suppressed && press->hold_ticket && press->hold_from_pointer) {
            ui_power_activity(display, now);
            if (g_ui.local_hold.fn(g_ui.local_hold.ctx, press->hold_ticket, press->hold_value,
                                   XIAOTAI_UI_HOLD_REFRESH) != XIAOTAI_OK) suppress(display, now);
        }
        return;
    }
    if (input == XIAOTAI_UI_INPUT_RELEASE && press->hold_ticket && press->hold_from_pointer && !same) return;
    if (input == XIAOTAI_UI_INPUT_PRESS && !display->power.pointer) activity(display, now);
    const bool touch = ui_power_touch(display, input, now);
    if (input == XIAOTAI_UI_INPUT_RELEASE && press->hold_ticket) {
        /* Removal or displacement of the original widget must not strand the
         * single pointer's accepted local HOLD. */
        press->down = false;
        ui_input_end_local_hold(display);
        return;
    }
    if (!touch) return;
    if (input == XIAOTAI_UI_INPUT_RELEASE) {
        if (press->down && (same || element == XIAOTAI_UI_ELEMENT_OTHER)) {
            if (!press->release_pending) release_hold(display, now);
            press->down = false;
        }
        return;
    }
    const xiaotai_ui_layer_t *out = ui_scene_layer(display, layer);
    if (!out || !out->present || layer != ui_scene_top(display)) {
        if (input == XIAOTAI_UI_INPUT_PRESS && layer <= XIAOTAI_LAYER_TOAST) activity(display, now);
        ui_input_ignored(display);
        return;
    }
    if (input == XIAOTAI_UI_INPUT_PRESS) {
        if (press->down) return;
        press->layer = layer;
        press->element = element;
        press->index = index;
        press->down = true;
        press->swiped = press->suppressed = press->long_fired = false;
        activity(display, now);
    } else if (input >= XIAOTAI_UI_INPUT_SWIPE_LEFT && input <= XIAOTAI_UI_INPUT_SWIPE_DOWN) {
        if (!press->down) {
            press->down = true;
            press->layer = layer;
            press->element = element;
            press->index = index;
            press->swiped = press->suppressed = false;
            activity(display, now);
        }
        swipe(display, layer, input, now);
        return;
    } else if (press->suppressed || (press->down && !same && press->element != XIAOTAI_UI_ELEMENT_OTHER)) {
        ui_input_ignored(display);
        return;
    }

    const xiaotai_view_t *view = &out->view;
    switch (element) {
        case XIAOTAI_UI_ELEMENT_ACTION:
            if (index >= view->action_count) break;
            action_input(display, layer, view, &view->actions[index], input, now);
            return;
        case XIAOTAI_UI_ELEMENT_ITEM:
            if (index >= view->item_count) break;
            if (input == XIAOTAI_UI_INPUT_TAP) {
                const xiaotai_view_item_t *item = &view->items[index];
                if (!available(item->state)) break;
                ui_scene_focus(display, layer, XIAOTAI_UI_FOCUS_ITEM, index);
                (void)send(display, item->intent, item->value, NULL);
            }
            return;
        case XIAOTAI_UI_ELEMENT_FIELD:
            if (index >= view->field_count) break;
            const xiaotai_view_field_t *field = &view->fields[index];
            if (input == XIAOTAI_UI_INPUT_CONFIRM_CHOICE) {
                if (field->kind != XIAOTAI_FIELD_CHOICE || !(field->flags & XIAOTAI_FIELD_CONFIRM_CURRENT) ||
                    !available(field->state) || value != field->value || value < 0 ||
                    (size_t)value >= field->option_count) break;
                if (send(display, field->intent, value, XIAOTAI_ARG_CHOICE_RECONFIRM))
                    ui_scene_focus(display, layer, XIAOTAI_UI_FOCUS_FIELD, index);
            } else if (input == XIAOTAI_UI_INPUT_CHANGE || input == XIAOTAI_UI_INPUT_COMMIT) {
                if (post_field(display, field, input == XIAOTAI_UI_INPUT_CHANGE, value, text)) {
                    ui_scene_focus(display, layer, XIAOTAI_UI_FOCUS_FIELD, index);
                }
            }
            return;
        case XIAOTAI_UI_ELEMENT_INDICATOR:
            if (index >= view->indicator_count) break;
            if (input == XIAOTAI_UI_INPUT_TAP) {
                const xiaotai_view_indicator_t *indicator = &view->indicators[index];
                (void)send(display, indicator->intent, indicator->value, NULL);
            }
            return;
        case XIAOTAI_UI_ELEMENT_OTHER:
            if (layer == XIAOTAI_LAYER_OVERLAY && input == XIAOTAI_UI_INPUT_TAP)
                role_action(display, view, XIAOTAI_ROLE_BACK);
            return;
        default: break;
    }
    ui_input_ignored(display);
}

void xiaotai_ui_host_post(const xiaotai_ui_host_t *host, const char *intent, int32_t value, const char *text)
{
    ui_display_t *display = host ? ui_display_at(host->index) : NULL;
    if (!display || host != &display->host) return;
    if (display->press.suppressed || display->power.suppressed || !display->power.awake) {
        ui_input_ignored(display);
        return;
    }
    (void)send(display, intent, value, text);
}
