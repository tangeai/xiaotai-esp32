// SPDX-License-Identifier: MIT
#include "companion_internal.h"

#include <string.h>

#include "xiaotai/hal.h"
#include "xiaotai/port/mem.h"

companion_t companion;
static const xiaotai_app_companion_style_t *s_product_styles[XIAOTAI_APP_COMPANION_STYLES_MAX];
static size_t s_product_style_count;

xiaotai_err_t xiaotai_app_companion_configure(const xiaotai_app_companion_config_t *config)
{
    if (companion.initialized) return XIAOTAI_ERR_INVALID_STATE;
#ifndef COMPANION_WITH_TRANSPORT
    if (config && config->home_online_requires_transport) return XIAOTAI_ERR_NOT_SUPPORTED;
#endif
    if (config && (config->contacts_per_page >= XIAOTAI_VIEW_ITEMS_MAX ||
                   config->requests_per_page >= XIAOTAI_VIEW_ITEMS_MAX || config->announce_repeat_s > 3600U ||
                   config->binding_retry_control > XIAOTAI_COMPANION_BINDING_RETRY_REFRESH ||
                   config->ai_contact_tools > XIAOTAI_COMPANION_AI_CONTACT_TOOLS_STANDARD)) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    companion.config = config ? *config : (xiaotai_app_companion_config_t){0};
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_app_companion_style_register(const xiaotai_app_companion_style_t *style)
{
    if (!style || !style->name || !style->name[0] || strlen(style->name) >= sizeof(companion.status.style) ||
        (style->needs_all & style->needs_none)) return XIAOTAI_ERR_INVALID_ARG;
    if (companion.initialized) return XIAOTAI_ERR_INVALID_STATE;
    for (size_t i = 0; i < s_product_style_count; ++i) {
        if (!strcmp(s_product_styles[i]->name, style->name)) return XIAOTAI_ERR_INVALID_STATE;
    }
    if (s_product_style_count >= XIAOTAI_APP_COMPANION_STYLES_MAX) return XIAOTAI_ERR_NO_MEM;
    s_product_styles[s_product_style_count++] = style;
    return XIAOTAI_OK;
}

static bool fits(const xiaotai_app_companion_style_t *style, uint32_t needs)
{
    return (style->needs_all & needs) == style->needs_all && !(style->needs_none & needs);
}

static const xiaotai_app_companion_style_t *choose_style(uint32_t needs, const char *name)
{
    for (size_t i = 0; i < s_product_style_count; ++i) {
        const xiaotai_app_companion_style_t *style = s_product_styles[i];
        if (name ? !strcmp(style->name, name) : fits(style, needs)) return style;
    }
    for (size_t i = 0; companion_builtins[i]; ++i) {
        const xiaotai_app_companion_style_t *style = companion_builtins[i];
        if (name ? !strcmp(style->name, name) : fits(style, needs)) return style;
    }
    return NULL;
}

uint32_t companion_toast_ms(void)
{
    return companion.style->toast_ms ? companion.style->toast_ms : XIAOTAI_TOAST_DEFAULT_MS;
}

void companion_report_refusal(const char *target, xiaotai_err_t error)
{
    xiaotai_args_t args = {.value = error};
    if (target) {
        const size_t length = strnlen(target, sizeof(args.text) - 1);
        memcpy(args.text, target, length);
    }
    ++companion.status.refused;
    (void)xiaotai_event_emit(XIAOTAI_EVENT_APP_COMPANION_REFUSED, &args, NULL, 0);
}

void companion_refuse_text(const char *target, xiaotai_err_t error, const char *text)
{
    companion_report_refusal(target, error);
    if (companion.status.foreground && companion.has_display) {
        xiaotai_view_toast(text ? text : companion.style->refused_text ?
                           companion.style->refused_text : "操作未受理，请重试", companion_toast_ms());
    }
}

void companion_refuse(const char *target, xiaotai_err_t error) { companion_refuse_text(target, error, NULL); }

void companion_user_activity(void)
{
    if (!companion.status.foreground) return;
    for (size_t i = 0; xiaotai_app_companion_parts[i]; ++i) {
        const xiaotai_app_companion_part_t *part = xiaotai_app_companion_parts[i];
        if (part->user_activity) part->user_activity();
    }
}

bool companion_screen_awake(void)
{
    for (size_t i = 0; xiaotai_app_companion_parts[i]; ++i) {
        const xiaotai_app_companion_part_t *part = xiaotai_app_companion_parts[i];
        if (part->screen_awake) return part->screen_awake();
    }
    return true;
}

static void on_main(const xiaotai_args_t *args, void *unused)
{
    (void)unused;
    if (companion.status.setup) {
        companion_refuse(XIAOTAI_INTENT_APP_COMPANION_MAIN, XIAOTAI_ERR_BUSY);
        return;
    }
    /* A visible or headless call owns the main action before an idle AI
     * provider has an opportunity to interpret it as a start request. */
    for (size_t i = 0; xiaotai_app_companion_parts[i]; ++i) {
        const xiaotai_app_companion_part_t *part = xiaotai_app_companion_parts[i];
        companion_home_session_t session;
        if (!part->main || !part->home_session || !part->home_session(&session) ||
            (session.mode != COMPANION_HOME_RINGING && session.mode != COMPANION_HOME_CALL_CONNECT &&
             session.mode != COMPANION_HOME_CALL)) continue;
        const xiaotai_err_t err = part->main(args->value);
        if (err == XIAOTAI_ERR_NOT_SUPPORTED) continue;
        if (err != XIAOTAI_OK) companion_refuse(XIAOTAI_INTENT_APP_COMPANION_MAIN, err);
        return;
    }
    for (size_t i = 0; xiaotai_app_companion_parts[i]; ++i) {
        const xiaotai_app_companion_part_t *part = xiaotai_app_companion_parts[i];
        if (!part->main) continue;
        const xiaotai_err_t err = part->main(args->value);
        if (err == XIAOTAI_ERR_NOT_SUPPORTED) continue;
        if (err != XIAOTAI_OK) companion_refuse(XIAOTAI_INTENT_APP_COMPANION_MAIN, err);
        return;
    }
    companion_refuse(XIAOTAI_INTENT_APP_COMPANION_MAIN, XIAOTAI_ERR_NOT_SUPPORTED);
}

bool xiaotai_app_companion_claim(const xiaotai_event_t *event)
{
    if (!event || !event->name || !companion.initialized) return false;
    if (companion_setup_claim(event)) return true;
    for (size_t i = 0; xiaotai_app_companion_parts[i]; ++i) {
        const xiaotai_app_companion_part_t *part = xiaotai_app_companion_parts[i];
        if (part->claim && part->claim(event)) return true;
    }
    return false;
}

xiaotai_err_t xiaotai_app_companion_init(void)
{
    if (companion.initialized) return XIAOTAI_ERR_INVALID_STATE;
    const uint32_t needs = xiaotai_hal_needs();
    const xiaotai_app_companion_style_t *style = choose_style(needs, companion.config.style);
    if (!style) return companion.config.style ? XIAOTAI_ERR_NOT_FOUND : XIAOTAI_ERR_NOT_SUPPORTED;
    if (!fits(style, needs)) return XIAOTAI_ERR_NOT_SUPPORTED;
    companion.style = style;
    companion.has_display = (needs & XIAOTAI_NEED_DISPLAY) != 0;
    strcpy(companion.status.style, style->name);
    if (!companion.config.title) companion.config.title = "小钛";
    if (!companion.config.contacts_per_page) companion.config.contacts_per_page = 8;
    if (!companion.config.requests_per_page) companion.config.requests_per_page = 4;

    /* The registration boundary is init, including failures after a partial registration. */
    companion.initialized = true;
    const xiaotai_interaction_profile_t *profile = companion_interaction_profile(style, companion.config.main_button_on_press);
    xiaotai_err_t err = profile ? xiaotai_interaction_register(profile) : XIAOTAI_OK;
    if (err == XIAOTAI_OK && style->feedback) err = xiaotai_feedback_register(style->feedback);
    for (size_t i = 0; xiaotai_app_companion_parts[i] && err == XIAOTAI_OK; ++i) {
        const xiaotai_app_companion_part_t *part = xiaotai_app_companion_parts[i];
        if (part->init) err = part->init();
    }
    if (err == XIAOTAI_OK) err = companion_settings_init();
    if (err == XIAOTAI_OK) err = companion_home_init();
    if (err == XIAOTAI_OK) err = companion_aux_init();
    if (err == XIAOTAI_OK && companion.has_display &&
        (style->policy & XIAOTAI_APP_COMPANION_HOME_NETWORK_STATUS)) err = companion_audio_init();
    if (err == XIAOTAI_OK) err = companion_pages_init();
    if (err == XIAOTAI_OK) err = companion_setup_init();
    if (err == XIAOTAI_OK) err = companion_session_init();
    if (err == XIAOTAI_OK) err = companion_at_init();
    if (err == XIAOTAI_OK) err = companion_feedback_init();
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_COMPANION_MAIN, on_main, NULL);
    if (err != XIAOTAI_OK) return err;
    if (companion.has_display) {
        companion.buffer = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*companion.buffer));
        if (!companion.buffer) {
            companion_setup_reset();
            return XIAOTAI_ERR_NO_MEM;
        }
    }
    return XIAOTAI_OK;
}

void xiaotai_app_companion_start(void)
{
    if (companion.initialized && (!companion.has_display || companion.buffer)) {
        (void)companion_reset_home(0);
        companion_setup_refresh();
    }
}

void xiaotai_app_companion_foreground(bool foreground)
{
    const bool leaving_result = !foreground && companion_on_page(XIAOTAI_APP_COMPANION_PAGE_RESULT);
    companion.status.foreground = foreground;
    if (!foreground) {
        companion_confirm_cancel(NULL);
        companion_session_result_left();
        if (leaving_result) companion_back_page();
        for (size_t i = 0; xiaotai_app_companion_parts[i]; ++i) {
            if (xiaotai_app_companion_parts[i]->foreground) xiaotai_app_companion_parts[i]->foreground(false);
        }
        companion.code_visible = false;
        if (!strcmp(companion.status.page, XIAOTAI_APP_COMPANION_PAGE_AUDIO)) companion_audio_leave();
        companion.status.page[0] = '\0';
        companion_invalidate_entries();
        companion_home_visible();
        companion_settings_visible();
        companion_feedback_visible();
        companion_aux_visible();
        return;
    }
    companion.status.context[0] = '\0';  /* another app may have set the global input context */
    companion_session_refresh_context();
    const xiaotai_app_entry_t entry = xiaotai_app_entry();
    if (!companion.depth || entry == XIAOTAI_APP_ENTRY_NEXT || entry == XIAOTAI_APP_ENTRY_PREVIOUS) {
        const bool end = entry == XIAOTAI_APP_ENTRY_PREVIOUS && (companion.style->policy & XIAOTAI_APP_COMPANION_CLOCK_HOME);
        companion_reset_home(end ? 1 : 0);
    }
    if (!companion.status.page[0]) companion_render();
    companion_home_visible();
    companion_settings_visible();
    companion_feedback_visible();
    companion_aux_visible();
    companion_setup_visible();
    for (size_t i = 0; xiaotai_app_companion_parts[i]; ++i) {
        if (xiaotai_app_companion_parts[i]->foreground) xiaotai_app_companion_parts[i]->foreground(true);
    }
}

xiaotai_err_t companion_show_wechat_code(void)
{
    if (!xiaotai_on_owner() || !companion.status.foreground) return XIAOTAI_ERR_INVALID_STATE;
    if (companion.status.setup || companion.confirm_resolved) return XIAOTAI_ERR_BUSY;
    for (size_t i = 0; xiaotai_app_companion_parts[i]; ++i) {
        const xiaotai_app_companion_part_t *part = xiaotai_app_companion_parts[i];
        if (part->show_wechat_code) return part->show_wechat_code();
    }
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

void xiaotai_app_companion_status(xiaotai_app_companion_status_t *status)
{
    if (status) *status = companion.status;
}

void xiaotai_app_companion_reset(void)
{
    for (size_t i = 0; xiaotai_app_companion_parts[i]; ++i) {
        if (xiaotai_app_companion_parts[i]->reset) xiaotai_app_companion_parts[i]->reset();
    }
    companion_session_reset();
    companion_setup_reset();
    companion_home_reset();
    companion_settings_reset();
    companion_feedback_reset();
    companion_aux_reset();
    companion_audio_reset();
    xiaotai_mem_free(companion.buffer);
    memset(&companion, 0, sizeof(companion));
    s_product_style_count = 0;
}
