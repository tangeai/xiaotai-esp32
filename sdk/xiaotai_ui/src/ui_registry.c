// SPDX-License-Identifier: MIT
/* Registries of themes, custom templates and images.
 *
 * Three tables of pointers, XIAOTAI_MEM_BULK, sized by the build options
 * (xiaotai/ui_config.h) and allocated at engine init. Registration happens
 * during init on the owner task; afterwards the tables are only read, which
 * is why lookups are [any] without a lock. Registering beyond a capacity is
 * an error in the registering init, so the start fails loudly instead of
 * losing an entry. */
#include <string.h>
#include "ui_internal.h"
#include "xiaotai/port/mem.h"

xiaotai_err_t ui_registry_init(void)
{
    g_ui.themes = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, XIAOTAI_UI_THEMES_MAX, sizeof(*g_ui.themes));
    g_ui.templates = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, XIAOTAI_UI_TEMPLATES_MAX, sizeof(*g_ui.templates));
    g_ui.images = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, XIAOTAI_UI_IMAGES_MAX, sizeof(*g_ui.images));
    if (!g_ui.themes || !g_ui.templates || !g_ui.images) {
        ui_registry_free();
        return XIAOTAI_ERR_NO_MEM;
    }
    return XIAOTAI_OK;
}

void ui_registry_free(void)
{
    xiaotai_mem_free((void *)g_ui.themes);
    xiaotai_mem_free((void *)g_ui.templates);
    xiaotai_mem_free((void *)g_ui.images);
    g_ui.themes = NULL;
    g_ui.templates = NULL;
    g_ui.images = NULL;
    g_ui.theme_count = 0U;
    g_ui.template_count = 0U;
    g_ui.image_count = 0U;
}

/* Registration is open between engine init and start. */
static bool registration_open(void) { return g_ui.initialized && !g_ui.started; }

/* -------------------------------------------------------------------- themes */

int ui_registry_score(const xiaotai_theme_t *theme, const xiaotai_display_desc_t *desc)
{
    int score = theme->match(desc);
    return score > 0 ? score : 0;
}

const xiaotai_theme_t *ui_registry_theme_named(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < g_ui.theme_count; ++i) {
        if (strcmp(g_ui.themes[i]->name, name) == 0) return g_ui.themes[i];
    }
    return NULL;
}

xiaotai_err_t xiaotai_ui_theme_register(const xiaotai_theme_t *theme)
{
    if (!theme || !theme->name || !theme->match || !theme->start || !theme->render ||
        theme->name[0] == '\0' || strnlen(theme->name, UI_THEME_NAME_MAX + 1U) > UI_THEME_NAME_MAX ||
        (theme->backend != XIAOTAI_UI_BACKEND_LVGL && theme->backend != XIAOTAI_UI_BACKEND_FRAME)) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (!registration_open() || ui_registry_theme_named(theme->name)) return XIAOTAI_ERR_INVALID_STATE;
    if (g_ui.theme_count >= XIAOTAI_UI_THEMES_MAX) return XIAOTAI_ERR_NO_MEM;
    g_ui.themes[g_ui.theme_count++] = theme;
    return XIAOTAI_OK;
}

size_t xiaotai_ui_theme_count(void) { return g_ui.themes ? g_ui.theme_count : 0U; }

const xiaotai_theme_t *xiaotai_ui_theme_at(size_t index)
{
    return g_ui.themes && index < g_ui.theme_count ? g_ui.themes[index] : NULL;
}

const xiaotai_theme_t *xiaotai_ui_theme_select(const xiaotai_display_desc_t *display)
{
    if (!display || !g_ui.themes) return NULL;
    const xiaotai_theme_t *best = NULL;
    int best_score = 0;
    for (size_t i = 0; i < g_ui.theme_count; ++i) {
        int score = ui_registry_score(g_ui.themes[i], display);
        /* Strictly greater: on a tie the theme registered first stays. */
        if (score > best_score) {
            best = g_ui.themes[i];
            best_score = score;
        }
    }
    return best;
}

/* ----------------------------------------------------------------- templates */

xiaotai_err_t xiaotai_ui_template_register(const xiaotai_ui_template_t *template_)
{
    if (!template_ || !template_->name || template_->name[0] == '\0' || !template_->match || !template_->open ||
        !template_->update || !template_->close ||
        (template_->backend != XIAOTAI_UI_BACKEND_LVGL && template_->backend != XIAOTAI_UI_BACKEND_FRAME)) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (!registration_open()) return XIAOTAI_ERR_INVALID_STATE;
    for (size_t i = 0; i < g_ui.template_count; ++i) {
        /* Several templates may share a name; the same one twice is a mistake. */
        if (g_ui.templates[i] == template_) return XIAOTAI_ERR_INVALID_STATE;
    }
    if (g_ui.template_count >= XIAOTAI_UI_TEMPLATES_MAX) return XIAOTAI_ERR_NO_MEM;
    g_ui.templates[g_ui.template_count++] = template_;
    return XIAOTAI_OK;
}

const xiaotai_ui_template_t *xiaotai_ui_template_find(const xiaotai_ui_host_t *host, const char *name)
{
    if (!host || !name || !g_ui.templates) return NULL;
    const ui_display_t *display = ui_display_at(host->index);
    if (!display || !display->theme) return NULL;
    const xiaotai_ui_template_t *best = NULL;
    int best_score = 0;
    for (size_t i = 0; i < g_ui.template_count; ++i) {
        const xiaotai_ui_template_t *candidate = g_ui.templates[i];
        /* A template draws with the backend of the display's theme or not at all. */
        if (candidate->backend != display->theme->backend || strcmp(candidate->name, name) != 0) continue;
        int score = candidate->match(&host->part->desc);
        if (score > best_score) {
            best = candidate;
            best_score = score;
        }
    }
    return best;
}

/* -------------------------------------------------------------------- images */

/* A row fits in size_t even on 32-bit targets; the whole image may not. */
static bool image_fits(const xiaotai_ui_image_t *image)
{
    size_t row_bytes;
    switch (image->format) {
    case XIAOTAI_IMAGE_MONO1: row_bytes = ((size_t)image->width + 7U) / 8U; break;
    case XIAOTAI_IMAGE_RGB565: row_bytes = (size_t)image->width * 2U; break;
    case XIAOTAI_IMAGE_RGB565A8: row_bytes = (size_t)image->width * 3U; break;
    default: return false;
    }
    return image->height <= image->size / row_bytes;
}

xiaotai_err_t xiaotai_ui_image_register(const xiaotai_ui_image_t *image)
{
    if (!image || !image->name || image->name[0] == '\0' || !image->data || image->width == 0U ||
        image->height == 0U) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (!image_fits(image)) return XIAOTAI_ERR_INVALID_ARG;
    if (!registration_open() || xiaotai_ui_image_find(image->name)) return XIAOTAI_ERR_INVALID_STATE;
    if (g_ui.image_count >= XIAOTAI_UI_IMAGES_MAX) return XIAOTAI_ERR_NO_MEM;
    g_ui.images[g_ui.image_count++] = image;
    return XIAOTAI_OK;
}

const xiaotai_ui_image_t *xiaotai_ui_image_find(const char *name)
{
    if (!name || !g_ui.images) return NULL;
    for (size_t i = 0; i < g_ui.image_count; ++i) {
        if (strcmp(g_ui.images[i]->name, name) == 0) return g_ui.images[i];
    }
    return NULL;
}
