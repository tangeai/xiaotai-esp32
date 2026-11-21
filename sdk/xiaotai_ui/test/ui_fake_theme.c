// SPDX-License-Identifier: MIT
/* Fake themes and templates for the UI system's host tests. match() has no
 * context argument, so each fake has its own small trampolines indexing a
 * table of configurations. */
#include <string.h>
#include "ui_test.h"

typedef struct {
    ui_fake_theme_config_t config;
    xiaotai_theme_t theme;
    uint32_t starts, stops;
    const xiaotai_ui_host_t *hosts[UI_FAKE_DISPLAYS_MAX];
} fake_theme_t;

static fake_theme_t s_themes[UI_FAKE_THEMES_MAX];

static int score(size_t index, const xiaotai_display_desc_t *desc)
{
    const ui_fake_theme_config_t *config = &s_themes[index].config;
    if (desc->pixel != config->pixel || (desc->flags & XIAOTAI_DISPLAY_SLOW_REFRESH)) return 0;
    return desc->width == config->width && desc->height == config->height ? config->exact : config->other;
}

static xiaotai_err_t start(size_t index, const xiaotai_ui_host_t *host, void **state)
{
    fake_theme_t *fake = &s_themes[index];
    ++fake->starts;
    if (fake->config.start_result != XIAOTAI_OK) return fake->config.start_result;
    uint8_t display = xiaotai_ui_host_index(host);
    if (display < UI_FAKE_DISPLAYS_MAX) fake->hosts[display] = host;
    *state = fake;
    return XIAOTAI_OK;
}

static void render(void *state, const xiaotai_ui_scene_t *scene)
{
    (void)state;
    (void)scene;
}

static void stop(void *state)
{
    fake_theme_t *fake = state;
    ++fake->stops;
}

#define FAKE_THEME_FUNCTIONS(n)                                                                      \
    static int match_##n(const xiaotai_display_desc_t *desc) { return score(n, desc); }              \
    static xiaotai_err_t start_##n(const xiaotai_ui_host_t *host, void **state) { return start(n, host, state); }

FAKE_THEME_FUNCTIONS(0)
FAKE_THEME_FUNCTIONS(1)
FAKE_THEME_FUNCTIONS(2)
FAKE_THEME_FUNCTIONS(3)
FAKE_THEME_FUNCTIONS(4)
FAKE_THEME_FUNCTIONS(5)
FAKE_THEME_FUNCTIONS(6)
FAKE_THEME_FUNCTIONS(7)
FAKE_THEME_FUNCTIONS(8)
FAKE_THEME_FUNCTIONS(9)

static int (*const MATCH[UI_FAKE_THEMES_MAX])(const xiaotai_display_desc_t *) = {
    match_0, match_1, match_2, match_3, match_4, match_5, match_6, match_7, match_8, match_9};
static xiaotai_err_t (*const START[UI_FAKE_THEMES_MAX])(const xiaotai_ui_host_t *, void **) = {
    start_0, start_1, start_2, start_3, start_4, start_5, start_6, start_7, start_8, start_9};

const xiaotai_theme_t *ui_fake_theme(size_t index, const ui_fake_theme_config_t *config)
{
    if (index >= UI_FAKE_THEMES_MAX) return NULL;
    fake_theme_t *fake = &s_themes[index];
    memset(fake, 0, sizeof(*fake));
    fake->config = *config;
    fake->theme.name = config->name;
    fake->theme.backend = config->backend;
    fake->theme.match = MATCH[index];
    fake->theme.start = START[index];
    fake->theme.render = render;
    fake->theme.stop = stop;
    fake->theme.stack_bytes = 1024U;
    return &fake->theme;
}

uint32_t ui_fake_theme_starts(size_t index) { return index < UI_FAKE_THEMES_MAX ? s_themes[index].starts : 0U; }
uint32_t ui_fake_theme_stops(size_t index) { return index < UI_FAKE_THEMES_MAX ? s_themes[index].stops : 0U; }

const xiaotai_ui_host_t *ui_fake_theme_host(size_t index, uint8_t display)
{
    return index < UI_FAKE_THEMES_MAX && display < UI_FAKE_DISPLAYS_MAX ? s_themes[index].hosts[display] : NULL;
}

/* -------------------------------------------------------------- templates */

typedef struct {
    xiaotai_ui_template_t template_;
    uint16_t width;
    int exact, other;
} fake_template_t;

static fake_template_t s_templates[UI_FAKE_TEMPLATES_MAX];

static int template_score(size_t index, const xiaotai_display_desc_t *desc)
{
    return desc->width == s_templates[index].width ? s_templates[index].exact : s_templates[index].other;
}

static xiaotai_err_t template_open(const xiaotai_ui_host_t *host, const xiaotai_ui_canvas_t *canvas,
                                   const xiaotai_ui_layer_t *layer, void **state)
{
    (void)host;
    (void)canvas;
    (void)layer;
    *state = NULL;
    return XIAOTAI_OK;
}

static void template_update(void *state, const xiaotai_ui_canvas_t *canvas, const xiaotai_ui_layer_t *layer,
                            const xiaotai_ui_scene_t *scene)
{
    (void)state;
    (void)canvas;
    (void)layer;
    (void)scene;
}

static void template_close(void *state) { (void)state; }

#define FAKE_TEMPLATE_MATCH(n) \
    static int template_match_##n(const xiaotai_display_desc_t *desc) { return template_score(n, desc); }

FAKE_TEMPLATE_MATCH(0)
FAKE_TEMPLATE_MATCH(1)
FAKE_TEMPLATE_MATCH(2)
FAKE_TEMPLATE_MATCH(3)
FAKE_TEMPLATE_MATCH(4)
FAKE_TEMPLATE_MATCH(5)
FAKE_TEMPLATE_MATCH(6)
FAKE_TEMPLATE_MATCH(7)
FAKE_TEMPLATE_MATCH(8)
FAKE_TEMPLATE_MATCH(9)
FAKE_TEMPLATE_MATCH(10)
FAKE_TEMPLATE_MATCH(11)
FAKE_TEMPLATE_MATCH(12)
FAKE_TEMPLATE_MATCH(13)
FAKE_TEMPLATE_MATCH(14)
FAKE_TEMPLATE_MATCH(15)
FAKE_TEMPLATE_MATCH(16)
FAKE_TEMPLATE_MATCH(17)
FAKE_TEMPLATE_MATCH(18)
FAKE_TEMPLATE_MATCH(19)

static int (*const TEMPLATE_MATCH[UI_FAKE_TEMPLATES_MAX])(const xiaotai_display_desc_t *) = {
    template_match_0,  template_match_1,  template_match_2,  template_match_3,  template_match_4,
    template_match_5,  template_match_6,  template_match_7,  template_match_8,  template_match_9,
    template_match_10, template_match_11, template_match_12, template_match_13, template_match_14,
    template_match_15, template_match_16, template_match_17, template_match_18, template_match_19};

const xiaotai_ui_template_t *ui_fake_template(size_t index, const char *name, xiaotai_ui_backend_t backend,
                                              uint16_t width, int exact, int other)
{
    if (index >= UI_FAKE_TEMPLATES_MAX) return NULL;
    fake_template_t *fake = &s_templates[index];
    memset(fake, 0, sizeof(*fake));
    fake->width = width;
    fake->exact = exact;
    fake->other = other;
    fake->template_.name = name;
    fake->template_.backend = backend;
    fake->template_.match = TEMPLATE_MATCH[index];
    fake->template_.open = template_open;
    fake->template_.update = template_update;
    fake->template_.close = template_close;
    return &fake->template_;
}
