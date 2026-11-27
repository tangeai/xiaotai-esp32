// SPDX-License-Identifier: MIT
#include "companion_internal.h"
#include "xiaotai/port/os.h"

#include <stdio.h>
#include <string.h>

static const companion_page_t home_page, menu_page;
#define ENTRY_ROWS 64U
#define TALK_STOP_INTENT "talk.stop"
_Static_assert(XIAOTAI_VIEW_ITEMS_MAX <= ENTRY_ROWS, "entry token must encode every possible row");

static companion_location_t *current(void)
{
    return companion.depth ? &companion.stack[companion.depth - 1] : NULL;
}

static bool room_navigation_allowed(const companion_page_t *page)
{
    return page && (page == &home_page || page == &menu_page ||
        !strcmp(page->id, XIAOTAI_APP_COMPANION_PAGE_ROOM) ||
        !strcmp(page->id, XIAOTAI_APP_COMPANION_PAGE_ROOM_FORM));
}

bool companion_room_navigation_here(void)
{
    const companion_location_t *location = current();
    return location && room_navigation_allowed(location->page);
}

static const companion_page_t *find_page(const char *id, size_t length)
{
    for (unsigned i = 0; i < companion.page_count; ++i) {
        const companion_page_t *page = companion.pages[i];
        if (strlen(page->id) == length && !memcmp(page->id, id, length)) return page;
    }
    return NULL;
}

bool companion_page_exists(const char *id) { return id && find_page(id, strlen(id)); }

bool companion_on_page(const char *id)
{
    const companion_location_t *location = current();
    const companion_page_t *setup = companion_setup_page();
    const companion_page_t *page = setup ? setup : location ? location->page : NULL;
    return companion.status.foreground && page && !strcmp(page->id, id);
}

xiaotai_err_t companion_page_register(const companion_page_t *page)
{
    if (companion.buffer) return XIAOTAI_ERR_INVALID_STATE;
    if (!page || !page->id || !page->id[0] || strlen(page->id) >= sizeof(companion.status.page) ||
        !page->title || !page->build) return XIAOTAI_ERR_INVALID_ARG;
    if (find_page(page->id, strlen(page->id))) return XIAOTAI_ERR_INVALID_STATE;
    if (companion.page_count >= COMPANION_PAGES_MAX) return XIAOTAI_ERR_NO_MEM;
    companion.pages[companion.page_count++] = page;
    return XIAOTAI_OK;
}

static bool resolve_entry(const char *entry, companion_location_t *location)
{
    const char *separator = strchr(entry, ':');
    const size_t length = separator ? (size_t)(separator - entry) : strlen(entry);
    const companion_page_t *page = find_page(entry, length);
    if (!page || page == &home_page || page == &menu_page) return false;
    int32_t value = 0;
    if (separator) {
        if (!separator[1]) return false;
        for (const char *p = separator + 1; *p; ++p) {
            if (*p < '0' || *p > '9' || value > (INT32_MAX - (*p - '0')) / 10) return false;
            value = 10 * value + (*p - '0');
        }
    }
    if (page->arg_valid && !page->arg_valid(value)) return false;
    *location = (companion_location_t){.page = page, .arg = value};
    return true;
}

static const char *const *page_entries(const companion_page_t *page)
{
    if (page == &home_page) return companion.style->home_entries;
    return page == &menu_page ? companion.style->menu_entries : NULL;
}

static size_t list_entries(const companion_page_t *page, xiaotai_view_item_t *items)
{
    const char *const *entries = page_entries(page);
    size_t used = 0;
    for (size_t i = 0; entries && entries[i] && used < XIAOTAI_VIEW_ITEMS_MAX; ++i) {
        companion_location_t location;
        if (!resolve_entry(entries[i], &location)) continue;
        if (items) {
            const char *label = location.page->entry_title ? location.page->entry_title(location.arg) : location.page->title;
            items[used] = (xiaotai_view_item_t){.label = label, .icon = location.page->icon,
                .intent = COMPANION_ENTRY_INTENT, .value = (int32_t)(companion.epoch * ENTRY_ROWS + used),
                .state = companion_room_maintenance() && !room_navigation_allowed(location.page) ?
                         XIAOTAI_STATE_DISABLED : 0};
        }
        ++used;
    }
    return used;
}

static void stock_home(xiaotai_view_t *view, int32_t screen)
{
    const bool over = !screen && companion_home_over_clock();
    view->kind = screen || over ? XIAOTAI_VIEW_CLOCK : XIAOTAI_VIEW_HOME;
    view->title = companion.config.title;
    companion_home_status(view, screen);
    if (over) {
        view->flags |= XIAOTAI_VIEW_CALENDAR;
        if (companion_home_talking) view->flags |= XIAOTAI_VIEW_CONVERSATION;
        else view->text = NULL;
    }
    /* Nothing running: the clock has the screen to itself. */
    if (screen && (view->flags & XIAOTAI_VIEW_MAY_SLEEP)) view->flags |= XIAOTAI_VIEW_CALENDAR;
    /* Over the face too: its buttons need not take the bottom of the screen. */
    if (!screen && !over && companion_home_talking) view->flags |= XIAOTAI_VIEW_CONVERSATION;
    if (!screen && !over) {
        view->items = companion.buffer->items;
        view->item_count = list_entries(&home_page, companion.buffer->items);
    }
    xiaotai_view_action_t *actions = companion.buffer->actions;
    size_t count = 0;
    if (!screen && !over && xiaotai_intent_exists(COMPANION_HOME_START_INTENT)) {
        actions[count++] = (xiaotai_view_action_t){.role = XIAOTAI_ROLE_PRIMARY,
            .intent = COMPANION_HOME_START_INTENT, .state = (companion.style->policy &
            XIAOTAI_APP_COMPANION_OFFLINE_GATE) && !companion_home_network_up() ? XIAOTAI_STATE_DISABLED : 0};
    }
    if (list_entries(&menu_page, NULL)) {
        actions[count++] = (xiaotai_view_action_t){.icon = XIAOTAI_ICON_MENU, .intent = XIAOTAI_INTENT_SYSTEM_MENU};
    }
    if (companion_home_talking) {
        if (companion_volume_available()) actions[count++] = (xiaotai_view_action_t){.label = "音量",
            .icon = XIAOTAI_ICON_VOLUME, .intent = COMPANION_VOLUME_INTENT};
        actions[count++] = (xiaotai_view_action_t){.label = "结束", .icon = XIAOTAI_ICON_STOP,
            .intent = XIAOTAI_INTENT_APP_COMPANION_END, .style = XIAOTAI_STYLE_DESTRUCTIVE};
    } else if (!over && xiaotai_intent_exists(COMPANION_PHONE_INTENT)) {
        actions[count++] = (xiaotai_view_action_t){.icon = XIAOTAI_ICON_PHONE, .intent = COMPANION_PHONE_INTENT,
            .state = (companion.style->policy & XIAOTAI_APP_COMPANION_OFFLINE_GATE) &&
                !companion_home_network_up() ? XIAOTAI_STATE_DISABLED : 0};
    }
    /* Paging leads to the clock screen, or to the product's other applications; not away from what runs here. */
    if (!xiaotai_activity_active() && (companion_clock_home() || xiaotai_app_count() > 1)) {
        actions[count++] = (xiaotai_view_action_t){.icon = XIAOTAI_ICON_NEXT, .intent = XIAOTAI_INTENT_APP_COMPANION_HOME,
            .trigger = XIAOTAI_TRIGGER_SWIPE_LEFT, .value = 1};
        actions[count++] = (xiaotai_view_action_t){.icon = XIAOTAI_ICON_PREVIOUS, .intent = XIAOTAI_INTENT_APP_COMPANION_HOME,
            .trigger = XIAOTAI_TRIGGER_SWIPE_RIGHT, .value = -1};
    }
    view->actions = actions;
    view->action_count = count;
}

static void stock_menu(xiaotai_view_t *view, int32_t unused)
{
    (void)unused;
    view->kind = XIAOTAI_VIEW_LIST;
    view->items = companion.buffer->items;
    view->item_count = list_entries(&menu_page, companion.buffer->items);
    companion.buffer->actions[0] = (xiaotai_view_action_t){.icon = XIAOTAI_ICON_BACK, .role = XIAOTAI_ROLE_BACK,
        .intent = XIAOTAI_INTENT_APP_COMPANION_BACK};
    view->actions = companion.buffer->actions;
    view->action_count = 1;
}

static bool home_arg(int32_t screen)
{
    return screen == 0 || (screen == 1 && companion_clock_home());
}

static const companion_page_t home_page = {.id = XIAOTAI_APP_COMPANION_PAGE_HOME, .title = "首页",
    .build = stock_home, .arg_valid = home_arg};
static const companion_page_t menu_page = {.id = XIAOTAI_APP_COMPANION_PAGE_MENU, .title = "菜单", .build = stock_menu};

static void show_confirmation(void)
{
    if (companion.confirm_choice_label) {
        const xiaotai_view_item_t items[] = {
            {.label = "取消", .intent = XIAOTAI_INTENT_APP_COMPANION_CONFIRM},
            {.label = companion.confirm_choice_label, .intent = XIAOTAI_INTENT_APP_COMPANION_CONFIRM,
                .value = 1, .state = companion.confirm_enabled ? 0 : XIAOTAI_STATE_DISABLED},
        };
        const xiaotai_view_action_t back = {.role = XIAOTAI_ROLE_BACK, .intent = XIAOTAI_INTENT_APP_COMPANION_CONFIRM};
        const xiaotai_view_t overlay = {.kind = XIAOTAI_VIEW_LIST, .layer = XIAOTAI_LAYER_OVERLAY,
            .id = companion.confirm_id, .title = companion.confirm_title, .status = companion.confirm_text,
            .items = items, .item_count = 2, .actions = &back, .action_count = 1};
        xiaotai_view_show(&overlay);
        return;
    }
    const xiaotai_view_action_t actions[] = {
        {.label = "确认", .role = XIAOTAI_ROLE_PRIMARY, .intent = XIAOTAI_INTENT_APP_COMPANION_CONFIRM,
            .value = 1, .state = companion.confirm_enabled ? 0 : XIAOTAI_STATE_DISABLED},
        {.label = "取消", .role = XIAOTAI_ROLE_BACK, .intent = XIAOTAI_INTENT_APP_COMPANION_CONFIRM},
    };
    const xiaotai_view_t overlay = {.kind = XIAOTAI_VIEW_NOTICE, .layer = XIAOTAI_LAYER_OVERLAY,
        .id = companion.confirm_id, .title = companion.confirm_title, .text = companion.confirm_text,
        .actions = actions, .action_count = 2};
    xiaotai_view_show(&overlay);
}

bool companion_confirm_open_choice(const char *title, const char *text, const char *accept_label,
                                   void (*resolved)(bool))
{
    if (!title || !resolved || !xiaotai_on_owner() || !companion.has_display ||
        !companion.status.foreground || companion.code_visible || companion.confirm_resolved) return false;
    (void)snprintf(companion.confirm_id, sizeof(companion.confirm_id), "confirm.%08x",
                   (unsigned int)++companion.confirm_generation);
    companion.confirm_resolved = resolved;
    companion.confirm_title = title;
    companion.confirm_text = text;
    companion.confirm_choice_label = accept_label;
    companion.confirm_enabled = true;
    show_confirmation();
    return true;
}

bool companion_confirm_open(const char *title, const char *text, void (*resolved)(bool))
{
    return companion_confirm_open_choice(title, text, NULL, resolved);
}

void companion_confirm_enabled(void (*resolved)(bool), bool enabled)
{
    if (!resolved || resolved != companion.confirm_resolved || enabled == companion.confirm_enabled) return;
    companion.confirm_enabled = enabled;
    show_confirmation();
}

void companion_confirm_cancel(void (*resolved)(bool))
{
    if (!companion.confirm_resolved || (resolved && resolved != companion.confirm_resolved)) return;
    companion.confirm_resolved = NULL;
    companion.confirm_title = companion.confirm_text = companion.confirm_choice_label = NULL;
    xiaotai_view_dismiss(XIAOTAI_LAYER_OVERLAY, companion.confirm_id);
}

static void resolve_confirmation(const xiaotai_args_t *args, void *unused)
{
    (void)unused;
    void (*const resolved)(bool) = companion.confirm_resolved;
    if (!companion.status.foreground || !resolved) { ++companion.status.stale; return; }
    if (args->value != 0 && args->value != 1) {
        companion_refuse(XIAOTAI_INTENT_APP_COMPANION_CONFIRM, XIAOTAI_ERR_INVALID_ARG);
        return;
    }
    if (args->value == 1 && !companion.confirm_enabled) { ++companion.status.stale; return; }
    companion_confirm_cancel(resolved);
    resolved(args->value == 1);
}

void companion_render(void)
{
    const companion_location_t *location = current();
    const companion_page_t *takeover = companion_setup_page();
    if ((!location && !takeover) || !companion.status.foreground || !companion.buffer) return;
    xiaotai_view_t *view = &companion.buffer->view;
    const xiaotai_view_kind_t previous_kind = view->kind;
    const companion_page_t *page = takeover ? takeover : location->page;
    if (!strcmp(companion.status.page, XIAOTAI_APP_COMPANION_PAGE_AUDIO) &&
        strcmp(page->id, XIAOTAI_APP_COMPANION_PAGE_AUDIO)) companion_audio_leave();
    if (!strcmp(companion.status.page, XIAOTAI_APP_COMPANION_PAGE_RESULT) &&
        strcmp(page->id, XIAOTAI_APP_COMPANION_PAGE_RESULT)) companion_session_result_left();
    memset(view, 0, sizeof(*view));
    view->id = page->id;
    view->title = page->title;
    const uint16_t focus_on_return = takeover ? 0U : location->focus_on_return;
    view->focus = takeover ? 0U : (focus_on_return ? focus_on_return : location->focus);
#if XIAOTAI_UI_OWNER_FRAME_WORKSPACE
    const bool building_view = companion.building_view;
    companion.building_view = true;
#endif
    page->build(view, takeover ? 0 : location->arg);
    /* A menu, a list or a result left open must not keep the screen lit for
     * ever; a session, a confirmation and the set-up pages still do. */
    if (!takeover && page != &home_page && companion_page_may_sleep()) view->flags |= XIAOTAI_VIEW_MAY_SLEEP;
    for (size_t i = 0; xiaotai_app_companion_parts[i]; ++i) {
        if (xiaotai_app_companion_parts[i]->decorate_view) xiaotai_app_companion_parts[i]->decorate_view(view);
    }
    if (focus_on_return) view->flags |= XIAOTAI_VIEW_FOCUS_ON_ENTRY;
    if (page == &home_page && companion_room_maintenance()) {
        companion.buffer->actions[0] = (xiaotai_view_action_t){.icon = XIAOTAI_ICON_MENU, .intent = XIAOTAI_INTENT_SYSTEM_MENU};
        view->actions = companion.buffer->actions;
        view->action_count = 1;
    }
    const bool changed = view->kind != previous_kind || strcmp(companion.status.page, page->id);
    if (changed) companion_confirm_cancel(NULL);
    xiaotai_view_show(view);
#if XIAOTAI_UI_OWNER_FRAME_WORKSPACE
    companion.building_view = building_view;
#endif
    strcpy(companion.status.page, page->id);
    if (companion.code_visible && changed) companion_settings_code_visible(false);
    if (changed) {
        for (size_t i = 0; xiaotai_app_companion_parts[i]; ++i) {
            const xiaotai_app_companion_part_t *part = xiaotai_app_companion_parts[i];
            if (part->page_visible) part->page_visible(companion.status.page);
        }
    }
}

void companion_invalidate_entries(void)
{
    companion.epoch = (companion.epoch + 1U) & (INT32_MAX / ENTRY_ROWS);
}

void companion_presentation_changed(void)
{
    companion_invalidate_entries();
    if (!companion.has_display) return;
    ++companion.status.pages;
    if (companion.status.foreground) {
        xiaotai_args_t args = {0};
        const companion_page_t *takeover = companion_setup_page();
        const companion_location_t *location = current();
        if (!takeover && !location) return;
        strcpy(args.text, takeover ? takeover->id : location->page->id);
        (void)xiaotai_event_emit(XIAOTAI_EVENT_APP_COMPANION_PAGE, &args, NULL, 0);
        companion_render();
    }
    companion_home_visible();
    companion_settings_visible();
    companion_feedback_visible();
    companion_aux_visible();
}

bool companion_reset_home(int32_t screen)
{
    const companion_location_t *old = current();
    const bool changed = !old || old->page != &home_page || old->arg != screen;
    companion.depth = 1;
    companion.stack[0] = (companion_location_t){.page = &home_page, .arg = screen};
    if (changed) companion_presentation_changed();
    return changed;
}

static bool room_page(const companion_page_t *page)
{
    return page && (!strcmp(page->id, XIAOTAI_APP_COMPANION_PAGE_ROOM) ||
        !strcmp(page->id, XIAOTAI_APP_COMPANION_PAGE_ROOM_FORM));
}

static bool page_ends_talk(const companion_page_t *old, const companion_page_t *page)
{
    if (room_page(page)) return !room_page(old) && !companion.config.room_keeps_talk;
    if (!(companion.style->policy & XIAOTAI_APP_COMPANION_PAGES_END_TALK)) return false;
    static const char *const pages[] = {
        XIAOTAI_APP_COMPANION_PAGE_CONTACTS, XIAOTAI_APP_COMPANION_PAGE_CONTACT,
        XIAOTAI_APP_COMPANION_PAGE_EMOTIONS, XIAOTAI_APP_COMPANION_PAGE_EMOTION,
        XIAOTAI_APP_COMPANION_PAGE_SETTINGS, XIAOTAI_APP_COMPANION_PAGE_NETWORK,
    };
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i) {
        if (!strcmp(page->id, pages[i])) return true;
    }
    return false;
}

bool companion_remove_page(const char *id)
{
    if (!id || !strcmp(home_page.id, id)) return false;
    const companion_location_t *top = current();
    const bool top_changed = top && !strcmp(top->page->id, id);
    unsigned depth = 0;
    for (unsigned i = 0; i < companion.depth; ++i) {
        if (strcmp(companion.stack[i].page->id, id)) companion.stack[depth++] = companion.stack[i];
    }
    const bool removed = companion.depth != depth;
    companion.depth = depth;
    if (top_changed) companion_presentation_changed();
    return removed;
}

bool companion_pop_call_page(void) { return companion_remove_page(XIAOTAI_APP_COMPANION_PAGE_CALL); }

static xiaotai_err_t show(const companion_page_t *target, int32_t arg, uint16_t source_focus, bool focus_once)
{
    const bool room_maintenance = companion_room_maintenance();
    if ((companion.status.setup || room_maintenance) && !(room_maintenance && room_navigation_allowed(target)))
        return XIAOTAI_ERR_BUSY;
    if (!target || (target == &menu_page && !list_entries(target, NULL))) return XIAOTAI_ERR_NOT_FOUND;
    if (target->arg_valid && !target->arg_valid(arg)) return XIAOTAI_ERR_INVALID_ARG;
    if (!strcmp(target->id, XIAOTAI_APP_COMPANION_PAGE_CALL)) {
        (void)companion_remove_page(XIAOTAI_APP_COMPANION_PAGE_RESULT);
    }
    bool entered = false;
    if (target == &home_page) {
        companion_reset_home(arg);
    } else {
        companion_location_t *old = current();
        if (!old || old->page != target || old->arg != arg) {
            if (companion.depth == COMPANION_STACK_MAX) return XIAOTAI_ERR_NO_MEM;
            if (page_ends_talk(old ? old->page : NULL, target) && xiaotai_intent_exists(TALK_STOP_INTENT)) {
                const xiaotai_err_t error = xiaotai_intent_post(TALK_STOP_INTENT, NULL);
                if (error != XIAOTAI_OK) return error;
            }
            if (old) {
                old->focus_on_return = focus_once ? source_focus : 0U;
                if (!focus_once && source_focus) old->focus = source_focus;
            }
            companion.stack[companion.depth++] = (companion_location_t){.page = target, .arg = arg};
            companion_presentation_changed();
            entered = true;
        }
    }
    const xiaotai_err_t err = !companion.status.foreground ? xiaotai_app_switch("companion") : XIAOTAI_OK;
    if (err == XIAOTAI_OK && entered && target->entered) target->entered();
    return err;
}

xiaotai_err_t companion_show_page(const char *id, int32_t value)
{
    return show(find_page(id, strlen(id)), value, 0, false);
}

xiaotai_err_t companion_show_page_from_item(const char *id, int32_t value, uint16_t source_focus)
{
    return show(find_page(id, strlen(id)), value, source_focus, true);
}

static void on_show(const xiaotai_args_t *args, void *ctx)
{
    const char *id = ctx ? ctx : args->text;
    const size_t length = strnlen(id, XIAOTAI_ARG_TEXT_MAX);
    const xiaotai_err_t err = show(find_page(id, length), ctx ? 0 : args->value, 0, false);
    if (err != XIAOTAI_OK) companion_refuse(length < XIAOTAI_ARG_TEXT_MAX ? id : XIAOTAI_INTENT_APP_COMPANION_SHOW, err);
}

static bool foreground_only(void)
{
    if (companion.status.foreground) return true;
    ++companion.status.stale;
    return false;
}

static void on_entry(const xiaotai_args_t *args, void *unused)
{
    (void)unused;
    if (!foreground_only()) return;
    if (companion.status.setup && !(companion_room_maintenance() && companion_room_navigation_here())) {
        ++companion.status.stale;
        return;
    }
    const uint32_t value = (uint32_t)args->value;
    companion_location_t *location = current();
    if (!location || value / ENTRY_ROWS != companion.epoch || (location->page == &home_page && location->arg != 0)) {
        ++companion.status.stale;
        return;
    }
    const uint32_t row = value % ENTRY_ROWS;
    if (row >= XIAOTAI_VIEW_ITEMS_MAX) { ++companion.status.stale; return; }
    const char *const *entries = page_entries(location->page);
    size_t used = 0;
    for (size_t i = 0; entries && entries[i]; ++i) {
        companion_location_t target;
        if (!resolve_entry(entries[i], &target)) continue;
        if (used++ == row) {
            if (target.page->launch) {
                const xiaotai_err_t err = xiaotai_intent_post(target.page->launch, &(xiaotai_args_t){0});
                if (err != XIAOTAI_OK) companion_refuse(target.page->id, err);
                return;
            }
            const xiaotai_err_t err = show(target.page, target.arg, (uint16_t)(row + 1), false);
            if (err != XIAOTAI_OK) companion_refuse(target.page->id, err);
            return;
        }
    }
    ++companion.status.stale;
}

static void go_product_home(void)
{
    const char *home = xiaotai_app_home();
    if (home && strcmp(home, "companion")) {
        if (xiaotai_app_entry() == XIAOTAI_APP_ENTRY_PRESENT && xiaotai_app_return() == XIAOTAI_OK) return;
        (void)xiaotai_app_switch(home);
    } else {
        companion_reset_home(0);
    }
}

void companion_back_page(void)
{
    if (companion.depth > 1) {
        --companion.depth;
        companion_presentation_changed();
    } else {
        go_product_home();
    }
}

static void on_back(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!foreground_only()) return;
    if (args->value == COMPANION_SETUP_BACK && companion.config.provisioning_cancel_from_keys) {
        if (xiaotai_intent_from_external() || companion_setup_cancel_from_key() || !companion.status.setup) return;
    }
    if (!companion.status.setup || (companion_room_maintenance() && companion_room_navigation_here()))
        companion_back_page();
}

static xiaotai_err_t step_home(int direction)
{
    if (xiaotai_activity_active()) return XIAOTAI_ERR_BUSY;
    const int32_t last = companion_clock_home() ? 1 : 0;
    const companion_location_t *location = current();
    const int32_t screen = (location && location->page == &home_page ? location->arg : 0) + direction;
    if (screen >= 0 && screen <= last) {
        companion_reset_home(screen);
        return XIAOTAI_OK;
    }
    const xiaotai_err_t err = direction > 0 ? xiaotai_app_next() : xiaotai_app_previous();
    if (err != XIAOTAI_ERR_NOT_FOUND) return err;
    companion_reset_home(direction > 0 ? 0 : last);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_app_companion_page_step(int direction)
{
    if (direction != 1 && direction != -1) return XIAOTAI_ERR_INVALID_ARG;
    if (!xiaotai_on_owner() || !companion.status.foreground) return XIAOTAI_ERR_INVALID_STATE;
    if (!companion.has_display) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (companion.status.setup || companion.confirm_resolved || companion.code_visible ||
        !companion_on_page(XIAOTAI_APP_COMPANION_PAGE_HOME)) return XIAOTAI_ERR_BUSY;
    return step_home(direction);
}

/* When the pager brought the companion forward; 0: it did not. */
static int64_t s_paged_us;

void companion_pager_entered(bool paged) { s_paged_us = paged ? xiaotai_time_us() : 0; }

static void on_home(const xiaotai_args_t *args, void *unused)
{
    (void)unused;
    if (!foreground_only() || companion.status.setup) return;
    const int32_t direction = args->value;
    if (direction == 0) { go_product_home(); return; }
    if (direction != 1 && direction != -1) {
        companion_refuse(XIAOTAI_INTENT_APP_COMPANION_HOME, XIAOTAI_ERR_INVALID_ARG);
        return;
    }
    /* Paged here a moment ago: the rest of that tap is not a second step. */
    if (s_paged_us && xiaotai_time_us() - s_paged_us < COMPANION_PAGER_GUARD_US) return;
    (void)step_home(direction);
}

/* An entry of the home list that hands the device to the doorbell mode
 * (another application); it opens no page of its own. */
static void no_page(xiaotai_view_t *view, int32_t arg) { (void)view; (void)arg; }
static const companion_page_t station_page = {.id = "doorbell", .title = "门铃", .build = no_page,
    .launch = COMPANION_STATION_INTENT};

xiaotai_err_t companion_pages_init(void)
{
    xiaotai_err_t err = companion_page_register(&home_page);
    if (err == XIAOTAI_OK && companion.style->menu_entries) err = companion_page_register(&menu_page);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_COMPANION_SHOW, on_show, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_COMPANION_BACK, on_back, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_COMPANION_HOME, on_home, NULL);
    if (err == XIAOTAI_OK && companion.has_display)
        err = xiaotai_intent_handle_ex(XIAOTAI_INTENT_APP_COMPANION_CONFIRM, resolve_confirmation, NULL, XIAOTAI_INTENT_LOCAL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle_ex(COMPANION_ENTRY_INTENT, on_entry, NULL, XIAOTAI_INTENT_LOCAL);
    if (err == XIAOTAI_OK && companion.style->home_entries && xiaotai_intent_exists(COMPANION_STATION_INTENT))
        err = companion_page_register(&station_page);
    if (err == XIAOTAI_OK && list_entries(&menu_page, NULL)) {
        err = xiaotai_intent_handle(XIAOTAI_INTENT_SYSTEM_MENU, on_show, (void *)XIAOTAI_APP_COMPANION_PAGE_MENU);
    }
    if (err == XIAOTAI_OK && find_page(XIAOTAI_APP_COMPANION_PAGE_NETWORK, sizeof(XIAOTAI_APP_COMPANION_PAGE_NETWORK) - 1)) {
        err = xiaotai_intent_handle(XIAOTAI_INTENT_SYSTEM_NETWORK, on_show, (void *)XIAOTAI_APP_COMPANION_PAGE_NETWORK);
    }
    return err;
}
