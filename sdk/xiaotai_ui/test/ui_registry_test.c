// SPDX-License-Identifier: MIT
/* Registries (src/ui_registry.c) and the theme each display gets at start
 * (src/ui_core.c): the highest score wins and the first registered wins a
 * tie, 0 and negative scores never select; names are unique, at most 15
 * characters, and registration is open from engine init to start; the
 * capacities are the build options; the product's theme is used for
 * display 0 when it is registered and draws it, otherwise refused (recorded
 * in the event log and reported) and the best match used; a theme whose
 * backend is not built, whose start fails or whose display does not open
 * leaves the display closed; templates match by name, backend and score;
 * images by name, with their size checked against their format. */
#include <string.h>
#include "ui_internal.h"
#include "ui_test.h"
#include "xiaotai/eventlog.h"

static const ui_fake_theme_config_t MONO = {.name = "mono128", .backend = XIAOTAI_UI_BACKEND_FRAME, .width = 128,
                                            .height = 64, .pixel = XIAOTAI_PIXEL_MONO1_VLSB, .exact = 100,
                                            .other = 50};
/* Draws 128x64 too, a little worse. */
static const ui_fake_theme_config_t MONO_ALT = {.name = "monoalt", .backend = XIAOTAI_UI_BACKEND_FRAME,
                                                .width = 128, .height = 64, .pixel = XIAOTAI_PIXEL_MONO1_VLSB,
                                                .exact = 90, .other = 50};
/* Scores like MONO_ALT for scaled displays: ties with it there. */
static const ui_fake_theme_config_t MONO_TIE = {.name = "monotie", .backend = XIAOTAI_UI_BACKEND_FRAME,
                                                .width = 1, .height = 1, .pixel = XIAOTAI_PIXEL_MONO1_VLSB,
                                                .exact = 0, .other = 50};
static const ui_fake_theme_config_t LCD = {.name = "lcd320", .backend = XIAOTAI_UI_BACKEND_LVGL, .width = 320,
                                           .height = 240, .pixel = XIAOTAI_PIXEL_RGB565, .exact = 100};
static const ui_fake_theme_config_t NEGATIVE = {.name = "negative", .backend = XIAOTAI_UI_BACKEND_FRAME,
                                                .width = 128, .height = 64, .pixel = XIAOTAI_PIXEL_MONO1_VLSB,
                                                .exact = -5, .other = -5};

static int init_engine(const xiaotai_ui_config_t *config)
{
    xiaotai_ui_reset();
    if (config) CHECK(xiaotai_ui_configure(config) == XIAOTAI_OK);
    CHECK(xiaotai_engine_ui_init() == XIAOTAI_OK);
    return 0;
}

static xiaotai_ui_info_t info(uint8_t display)
{
    xiaotai_ui_info_t out;
    memset(&out, 0, sizeof(out));
    (void)xiaotai_ui_inspect(display, &out);
    return out;
}

/* The value of the newest "ui" event with this label, or -1000 when none. */
static int32_t last_event(const char *label)
{
    xiaotai_eventlog_entry_t entries[32];
    uint32_t latest = xiaotai_eventlog_latest();
    size_t count = xiaotai_eventlog_read(latest > 32U ? latest - 32U : 0U, entries, 32U);
    for (size_t i = count; i-- > 0;) {
        if (strcmp(entries[i].source, "ui") == 0 && strcmp(entries[i].label, label) == 0) return entries[i].value;
    }
    return -1000;
}

/* ------------------------------------------------------------------ themes */

static int test_theme_registration(void)
{
    xiaotai_ui_reset();
    /* Before the engine initialized there is no registry. */
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(0, &MONO)) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_ui_theme_count() == 0U && xiaotai_ui_theme_at(0) == NULL);
    const xiaotai_display_desc_t mono = ui_fake_desc(128, 64, XIAOTAI_PIXEL_MONO1_VLSB);
    CHECK(xiaotai_ui_theme_select(&mono) == NULL);

    CHECK(init_engine(NULL) == 0);
    CHECK(xiaotai_ui_theme_register(NULL) == XIAOTAI_ERR_INVALID_ARG);
    ui_fake_theme_config_t config = MONO;
    config.name = "sixteen_chars_xx";   /* 16 characters */
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(1, &config)) == XIAOTAI_ERR_INVALID_ARG);
    config.name = "";
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(1, &config)) == XIAOTAI_ERR_INVALID_ARG);
    config.name = "fifteen_chars_x";   /* 15 characters */
    xiaotai_theme_t broken = *ui_fake_theme(1, &config);
    broken.match = NULL;
    CHECK(xiaotai_ui_theme_register(&broken) == XIAOTAI_ERR_INVALID_ARG);
    broken = *ui_fake_theme(1, &config);
    broken.render = NULL;
    CHECK(xiaotai_ui_theme_register(&broken) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(1, &config)) == XIAOTAI_OK);
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(0, &MONO)) == XIAOTAI_OK);
    /* A name is taken once, whatever else differs. */
    config = LCD;
    config.name = "mono128";
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(2, &config)) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_ui_theme_count() == 2U);
    CHECK(strcmp(xiaotai_ui_theme_at(0)->name, "fifteen_chars_x") == 0);
    CHECK(strcmp(xiaotai_ui_theme_at(1)->name, "mono128") == 0 && xiaotai_ui_theme_at(2) == NULL);

    /* The capacity is the build option: one more is an error, not a loss. */
    static char names[UI_FAKE_THEMES_MAX][8];
    size_t index = 2;
    for (; xiaotai_ui_theme_count() < XIAOTAI_UI_THEMES_MAX; ++index) {
        snprintf(names[index], sizeof(names[index]), "t%zu", index);
        config = MONO;
        config.name = names[index];
        CHECK(xiaotai_ui_theme_register(ui_fake_theme(index, &config)) == XIAOTAI_OK);
    }
    snprintf(names[index], sizeof(names[index]), "t%zu", index);
    config.name = names[index];
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(index, &config)) == XIAOTAI_ERR_NO_MEM);
    CHECK(xiaotai_ui_theme_count() == XIAOTAI_UI_THEMES_MAX);

    /* Closed once the UI started. */
    xiaotai_engine_ui_start();
    CHECK(init_engine(NULL) == 0);
    xiaotai_engine_ui_start();
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(0, &MONO)) == XIAOTAI_ERR_INVALID_STATE);
    return 0;
}

static int test_theme_scoring(void)
{
    CHECK(init_engine(NULL) == 0);
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(0, &NEGATIVE)) == XIAOTAI_OK);
    const xiaotai_display_desc_t mono = ui_fake_desc(128, 64, XIAOTAI_PIXEL_MONO1_VLSB);
    const xiaotai_display_desc_t mono_big = ui_fake_desc(256, 64, XIAOTAI_PIXEL_MONO1_VLSB);
    const xiaotai_display_desc_t lcd = ui_fake_desc(320, 240, XIAOTAI_PIXEL_RGB565);
    const xiaotai_display_desc_t door = ui_fake_desc(1024, 600, XIAOTAI_PIXEL_RGB565);
    /* A negative score is no score. */
    CHECK(xiaotai_ui_theme_select(&mono) == NULL);

    CHECK(xiaotai_ui_theme_register(ui_fake_theme(1, &MONO_ALT)) == XIAOTAI_OK);
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(2, &MONO_TIE)) == XIAOTAI_OK);
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(3, &MONO)) == XIAOTAI_OK);
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(4, &LCD)) == XIAOTAI_OK);
    /* Highest score, wherever it was registered. */
    CHECK(strcmp(xiaotai_ui_theme_select(&mono)->name, "mono128") == 0);
    CHECK(strcmp(xiaotai_ui_theme_select(&lcd)->name, "lcd320") == 0);
    /* MONO_ALT, MONO_TIE and MONO all score 50 here: the first registered. */
    CHECK(strcmp(xiaotai_ui_theme_select(&mono_big)->name, "monoalt") == 0);
    /* Nobody draws it. */
    CHECK(xiaotai_ui_theme_select(&door) == NULL);
    CHECK(xiaotai_ui_theme_select(NULL) == NULL);
    /* A slow panel: these themes score 0 for it, so none is chosen. */
    xiaotai_display_desc_t slow = mono;
    slow.flags = XIAOTAI_DISPLAY_SLOW_REFRESH;
    CHECK(xiaotai_ui_theme_select(&slow) == NULL);
    return 0;
}

static xiaotai_ui_info_t s_while_starting;

static xiaotai_err_t inspect_before_theme_ready(const xiaotai_ui_host_t *host, void **state)
{
    *state = NULL;
    s_while_starting = info(xiaotai_ui_host_index(host));
    return XIAOTAI_OK;
}

static int test_running_theme_publication(void)
{
    CHECK(init_engine(NULL) == 0);
    xiaotai_theme_t theme = *ui_fake_theme(0, &MONO);
    theme.start = inspect_before_theme_ready;
    theme.stop = NULL;
    CHECK(xiaotai_ui_theme_register(&theme) == XIAOTAI_OK);
    xiaotai_engine_ui_start();
    CHECK(!s_while_starting.ready && !s_while_starting.theme[0]);
    CHECK(info(0).ready && !strcmp(info(0).theme, "mono128"));
    xiaotai_ui_reset();
    return 0;
}

/* --------------------------------------------------------- start and refusal */

static int test_start_picks_theme(void)
{
    xiaotai_display_desc_t descs[2] = {ui_fake_desc(128, 64, XIAOTAI_PIXEL_MONO1_VLSB),
                                       ui_fake_desc(128, 64, XIAOTAI_PIXEL_MONO1_VLSB)};
    ui_fake_board_set_displays(descs, 2);

    /* Best match on both displays. */
    CHECK(init_engine(NULL) == 0);
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(0, &MONO_ALT)) == XIAOTAI_OK);
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(1, &MONO)) == XIAOTAI_OK);
    xiaotai_engine_ui_start();
    CHECK(strcmp(info(0).theme, "mono128") == 0 && strcmp(info(1).theme, "mono128") == 0);
    CHECK(ui_fake_theme_starts(1) == 2U && ui_fake_theme_host(1, 0) != ui_fake_theme_host(1, 1));
    CHECK(xiaotai_ui_host_index(ui_fake_theme_host(1, 1)) == 1);
    CHECK(ui_fake_display_opens(0) == 1U && ui_fake_display_opens(1) == 1U);
    CHECK(!info(0).theme_refused);
    /* Configuring after start is refused. */
    const xiaotai_ui_config_t late = {.sleep_timeout_ms = 1000};
    CHECK(xiaotai_ui_configure(&late) == XIAOTAI_ERR_INVALID_STATE);

    /* The product's theme on display 0 although another scores higher; the
     * other displays keep the best match. */
    ui_fake_board_set_displays(descs, 2);
    const xiaotai_ui_config_t named = {.theme = "monoalt"};
    CHECK(init_engine(&named) == 0);
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(0, &MONO_ALT)) == XIAOTAI_OK);
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(1, &MONO)) == XIAOTAI_OK);
    xiaotai_engine_ui_start();
    CHECK(strcmp(info(0).theme, "monoalt") == 0 && strcmp(info(1).theme, "mono128") == 0);
    CHECK(!info(0).theme_refused && info(0).ready && info(1).ready);

    /* Unknown name: refused, recorded, and display 0 does not stay dark. */
    ui_fake_board_set_displays(descs, 1);
    const xiaotai_ui_config_t unknown = {.theme = "lcd480"};
    CHECK(init_engine(&unknown) == 0);
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(0, &MONO)) == XIAOTAI_OK);
    xiaotai_engine_ui_start();
    CHECK(info(0).theme_refused && info(0).ready && strcmp(info(0).theme, "mono128") == 0);
    CHECK(last_event("theme.refused") == 1);

    /* Registered but scoring 0 for the display: refused the same way. */
    const xiaotai_ui_config_t zero = {.theme = "lcd320"};
    CHECK(init_engine(&zero) == 0);
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(0, &LCD)) == XIAOTAI_OK);
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(1, &MONO)) == XIAOTAI_OK);
    xiaotai_engine_ui_start();
    CHECK(info(0).theme_refused && strcmp(info(0).theme, "mono128") == 0);
    CHECK(last_event("theme.refused") == 2);

    /* A name no theme can carry is refused when configured. */
    const xiaotai_ui_config_t too_long = {.theme = "sixteen_chars_xx"};
    xiaotai_ui_reset();
    CHECK(xiaotai_ui_configure(&too_long) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_ui_configure(NULL) == XIAOTAI_ERR_INVALID_ARG);
    const xiaotai_ui_config_t fifteen = {.theme = "fifteen_chars_x"};
    CHECK(xiaotai_ui_configure(&fifteen) == XIAOTAI_OK);
    /* The name is the product's to change after configure: it was copied. */
    char buffer[16];
    strcpy(buffer, "monoalt");
    const xiaotai_ui_config_t borrowed = {.theme = buffer};
    CHECK(xiaotai_ui_configure(&borrowed) == XIAOTAI_OK);
    strcpy(buffer, "mono128");
    CHECK(xiaotai_engine_ui_init() == XIAOTAI_OK);
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(0, &MONO)) == XIAOTAI_OK);
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(1, &MONO_ALT)) == XIAOTAI_OK);
    xiaotai_engine_ui_start();
    CHECK(strcmp(info(0).theme, "monoalt") == 0);
    return 0;
}

static int test_start_failures(void)
{
    const xiaotai_display_desc_t mono = ui_fake_desc(128, 64, XIAOTAI_PIXEL_MONO1_VLSB);
    const xiaotai_display_desc_t lcd = ui_fake_desc(320, 240, XIAOTAI_PIXEL_RGB565);

    /* The theme's own start fails: no theme on that display. */
    ui_fake_board_set_displays(&mono, 1);
    CHECK(init_engine(NULL) == 0);
    ui_fake_theme_config_t failing = MONO;
    failing.start_result = XIAOTAI_ERR_NO_MEM;
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(0, &failing)) == XIAOTAI_OK);
    xiaotai_engine_ui_start();
    CHECK(!info(0).ready && info(0).theme[0] == '\0' && ui_fake_theme_starts(0) == 1U);
    CHECK(last_event("theme.start_failed") == XIAOTAI_ERR_NO_MEM);
    xiaotai_view_t view = {.kind = XIAOTAI_VIEW_HOME};
    ui_view_accept(&view);
    CHECK(info(0).dropped == 1U);

    /* The part does not open. */
    ui_fake_board_set_displays(&mono, 1);
    ui_fake_display_fail_open(0, XIAOTAI_ERR_TIMEOUT);
    CHECK(init_engine(NULL) == 0);
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(0, &MONO)) == XIAOTAI_OK);
    xiaotai_engine_ui_start();
    CHECK(!info(0).ready && ui_fake_display_opens(0) == 1U && ui_fake_theme_starts(0) == 0U);
    CHECK(last_event("display.open_failed") == XIAOTAI_ERR_TIMEOUT);

    /* An LVGL theme where the LVGL backend is not built: the panel is not
     * even opened. */
#if !XIAOTAI_UI_LVGL
    ui_fake_board_set_displays(&lcd, 1);
    CHECK(init_engine(NULL) == 0);
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(0, &LCD)) == XIAOTAI_OK);
    xiaotai_engine_ui_start();
    CHECK(!info(0).ready && ui_fake_display_opens(0) == 0U && ui_fake_theme_starts(0) == 0U);
    CHECK(last_event("theme.no_backend") == 0);
#endif

    /* No theme at all. */
    ui_fake_board_set_displays(&lcd, 1);
    CHECK(init_engine(NULL) == 0);
    xiaotai_engine_ui_start();
    CHECK(!info(0).ready && last_event("display.no_theme") == 0);
    ui_fake_board_set_displays(&mono, 1);
    return 0;
}

/* --------------------------------------------------------------- templates */

static int test_templates(void)
{
    const xiaotai_display_desc_t descs[2] = {ui_fake_desc(128, 64, XIAOTAI_PIXEL_MONO1_VLSB),
                                             ui_fake_desc(1024, 600, XIAOTAI_PIXEL_MONO1_VLSB)};
    ui_fake_board_set_displays(descs, 2);
    CHECK(init_engine(NULL) == 0);
    CHECK(xiaotai_ui_theme_register(ui_fake_theme(0, &MONO)) == XIAOTAI_OK);

    const xiaotai_ui_template_t *wide = ui_fake_template(0, "door.home", XIAOTAI_UI_BACKEND_FRAME, 1024, 100, 0);
    const xiaotai_ui_template_t *any = ui_fake_template(1, "door.home", XIAOTAI_UI_BACKEND_FRAME, 0, 10, 10);
    const xiaotai_ui_template_t *lvgl = ui_fake_template(2, "door.home", XIAOTAI_UI_BACKEND_LVGL, 0, 200, 200);
    const xiaotai_ui_template_t *call = ui_fake_template(3, "door.call", XIAOTAI_UI_BACKEND_FRAME, 1024, 100, 0);
    CHECK(xiaotai_ui_template_register(wide) == XIAOTAI_OK);
    CHECK(xiaotai_ui_template_register(lvgl) == XIAOTAI_OK);
    CHECK(xiaotai_ui_template_register(any) == XIAOTAI_OK);
    CHECK(xiaotai_ui_template_register(call) == XIAOTAI_OK);
    /* Several templates share a name; the same one twice is refused. */
    CHECK(xiaotai_ui_template_register(any) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_ui_template_register(NULL) == XIAOTAI_ERR_INVALID_ARG);
    xiaotai_ui_template_t broken = *ui_fake_template(4, "x", XIAOTAI_UI_BACKEND_FRAME, 0, 1, 1);
    broken.open = NULL;
    CHECK(xiaotai_ui_template_register(&broken) == XIAOTAI_ERR_INVALID_ARG);
    xiaotai_engine_ui_start();

    const xiaotai_ui_host_t *small = ui_fake_theme_host(0, 0);
    const xiaotai_ui_host_t *large = ui_fake_theme_host(0, 1);
    CHECK(small && large);
    /* The LVGL template scores highest but cannot draw on a FRAME theme's display. */
    CHECK(xiaotai_ui_template_find(small, "door.home") == any);
    CHECK(xiaotai_ui_template_find(large, "door.home") == wide);
    /* Not for this display, or no such name: NULL, the theme draws NOTICE. */
    CHECK(xiaotai_ui_template_find(small, "door.call") == NULL);
    CHECK(xiaotai_ui_template_find(large, "door.call") == call);
    CHECK(xiaotai_ui_template_find(large, "door.missing") == NULL);
    CHECK(xiaotai_ui_template_find(NULL, "door.home") == NULL);
    CHECK(xiaotai_ui_template_find(small, NULL) == NULL);
    /* Registration closed after start. */
    CHECK(xiaotai_ui_template_register(ui_fake_template(5, "late", XIAOTAI_UI_BACKEND_FRAME, 0, 1, 1)) ==
          XIAOTAI_ERR_INVALID_STATE);

    /* Capacity. */
    ui_fake_board_set_displays(descs, 1);
    CHECK(init_engine(NULL) == 0);
    for (size_t i = 0; i < XIAOTAI_UI_TEMPLATES_MAX; ++i) {
        CHECK(xiaotai_ui_template_register(ui_fake_template(i, "t", XIAOTAI_UI_BACKEND_FRAME, 0, 1, 1)) ==
              XIAOTAI_OK);
    }
    CHECK(xiaotai_ui_template_register(ui_fake_template(XIAOTAI_UI_TEMPLATES_MAX, "t", XIAOTAI_UI_BACKEND_FRAME,
                                                        0, 1, 1)) == XIAOTAI_ERR_NO_MEM);
    return 0;
}

/* ------------------------------------------------------------------ images */

static const uint8_t PIXELS[16 * 16 * 3];

static int test_images(void)
{
    CHECK(init_engine(NULL) == 0);
    /* 16x16 one-bit: two bytes a row; 13 pixels wide still takes two. */
    const xiaotai_ui_image_t qr = {.name = "qr", .width = 16, .height = 16, .format = XIAOTAI_IMAGE_MONO1,
                                   .data = PIXELS, .size = 32};
    const xiaotai_ui_image_t narrow = {.name = "narrow", .width = 13, .height = 2, .format = XIAOTAI_IMAGE_MONO1,
                                       .data = PIXELS, .size = 4};
    const xiaotai_ui_image_t short_mono = {.name = "short", .width = 16, .height = 16,
                                           .format = XIAOTAI_IMAGE_MONO1, .data = PIXELS, .size = 31};
    const xiaotai_ui_image_t icon = {.name = "icon", .width = 10, .height = 10, .format = XIAOTAI_IMAGE_RGB565,
                                     .data = PIXELS, .size = 200};
    const xiaotai_ui_image_t alpha = {.name = "alpha", .width = 10, .height = 10, .format = XIAOTAI_IMAGE_RGB565A8,
                                      .data = PIXELS, .size = 300};
    xiaotai_ui_image_t alpha_short = alpha;
    alpha_short.name = "alpha_short";
    alpha_short.size = 299;
    xiaotai_ui_image_t no_data = qr;
    no_data.name = "no_data";
    no_data.data = NULL;
    xiaotai_ui_image_t empty = qr;
    empty.name = "empty";
    empty.width = 0;
    /* Actual RGB565 size is 4295032830; a 32-bit product must not accept the
     * 65534-byte remainder as the validated size of the borrowed image. */
    static uint8_t small_image[65534];
    const xiaotai_ui_image_t oversized = {.name = "oversized", .width = 32769, .height = 65535,
        .format = XIAOTAI_IMAGE_RGB565, .data = small_image, .size = sizeof(small_image)};

    CHECK(xiaotai_ui_image_find("qr") == NULL);
    CHECK(xiaotai_ui_image_register(&qr) == XIAOTAI_OK);
    CHECK(xiaotai_ui_image_register(&narrow) == XIAOTAI_OK);
    CHECK(xiaotai_ui_image_register(&icon) == XIAOTAI_OK);
    CHECK(xiaotai_ui_image_register(&alpha) == XIAOTAI_OK);
    CHECK(xiaotai_ui_image_register(&short_mono) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_ui_image_register(&alpha_short) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_ui_image_register(&no_data) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_ui_image_register(&empty) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_ui_image_register(&oversized) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_ui_image_find("oversized") == NULL);
    CHECK(xiaotai_ui_image_register(NULL) == XIAOTAI_ERR_INVALID_ARG);
    /* The name is taken. */
    xiaotai_ui_image_t again = icon;
    again.name = "qr";
    CHECK(xiaotai_ui_image_register(&again) == XIAOTAI_ERR_INVALID_STATE);
    /* Kept by pointer, found by name. */
    CHECK(xiaotai_ui_image_find("qr") == &qr && xiaotai_ui_image_find("alpha") == &alpha);
    CHECK(xiaotai_ui_image_find("short") == NULL && xiaotai_ui_image_find(NULL) == NULL);

    static char names[XIAOTAI_UI_IMAGES_MAX + 1][8];
    static xiaotai_ui_image_t more[XIAOTAI_UI_IMAGES_MAX + 1];
    size_t registered = 4;
    for (size_t i = 0; registered < XIAOTAI_UI_IMAGES_MAX; ++i, ++registered) {
        snprintf(names[i], sizeof(names[i]), "i%zu", i);
        more[i] = qr;
        more[i].name = names[i];
        CHECK(xiaotai_ui_image_register(&more[i]) == XIAOTAI_OK);
    }
    snprintf(names[XIAOTAI_UI_IMAGES_MAX], sizeof(names[0]), "last");
    more[XIAOTAI_UI_IMAGES_MAX] = qr;
    more[XIAOTAI_UI_IMAGES_MAX].name = names[XIAOTAI_UI_IMAGES_MAX];
    CHECK(xiaotai_ui_image_register(&more[XIAOTAI_UI_IMAGES_MAX]) == XIAOTAI_ERR_NO_MEM);

    /* Registration closed after start; lookups go on. */
    xiaotai_engine_ui_start();
    xiaotai_ui_image_t late = qr;
    late.name = "late";
    CHECK(xiaotai_ui_image_register(&late) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_ui_image_find("narrow") == &narrow);

    /* Reset forgets every registration. */
    xiaotai_ui_reset();
    CHECK(xiaotai_ui_image_find("qr") == NULL && xiaotai_ui_theme_count() == 0U);
    return 0;
}

int main(void)
{
    const xiaotai_display_desc_t mono = ui_fake_desc(128, 64, XIAOTAI_PIXEL_MONO1_VLSB);
    ui_fake_board_set_displays(&mono, 1);
    CHECK(xiaotai_hal_init() == XIAOTAI_OK);
    CHECK(xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
    RUN(test_theme_registration);
    RUN(test_theme_scoring);
    RUN(test_running_theme_publication);
    RUN(test_start_picks_theme);
    RUN(test_start_failures);
    RUN(test_templates);
    RUN(test_images);
    xiaotai_ui_reset();
    return 0;
}
