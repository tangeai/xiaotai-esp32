// SPDX-License-Identifier: MIT
/* The actors that registered with the kit (xiaotai/theme_lcd.h). The list
 * needs no drawing library, so it is built on every platform. */
#include "xiaotai/theme_lcd.h"

static const xiaotai_theme_lcd_actor_t *actors[XIAOTAI_THEME_LCD_ACTORS_MAX];
static size_t actor_count;

xiaotai_err_t xiaotai_theme_lcd_actor_register(const xiaotai_theme_lcd_actor_t *actor)
{
    if (!actor || !actor->name || !actor->match || !actor->start || !actor->stop || !actor->build || !actor->place ||
        !actor->detach || !actor->set || !actor->tick || !actor->hit) return XIAOTAI_ERR_INVALID_ARG;
    for (size_t i = 0; i < actor_count; ++i)
        if (actors[i] == actor) return XIAOTAI_OK;
    if (actor_count >= XIAOTAI_THEME_LCD_ACTORS_MAX) return XIAOTAI_ERR_NO_MEM;
    actors[actor_count++] = actor;
    return XIAOTAI_OK;
}

const xiaotai_theme_lcd_actor_t *xiaotai_theme_lcd_actor_select(const xiaotai_display_desc_t *display)
{
    const xiaotai_theme_lcd_actor_t *best = NULL;
    int score = 0;
    for (size_t i = 0; display && i < actor_count; ++i) {
        const int now = actors[i]->match(display);
        if (now > score) {
            score = now;
            best = actors[i];
        }
    }
    return best;
}

void xiaotai_theme_lcd_actor_reset(void)
{
    actor_count = 0;
}
