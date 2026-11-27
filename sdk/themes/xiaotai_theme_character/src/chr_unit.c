// SPDX-License-Identifier: MIT
#include "xiaotai/theme_character.h"

#include "chr_director.h"
#include "xiaotai/theme_lcd.h"

xiaotai_err_t xiaotai_engine_theme_character_init(void)
{
    const xiaotai_err_t err = chr_director_init();
    return err != XIAOTAI_OK ? err : xiaotai_theme_lcd_actor_register(&xiaotai_theme_character_actor);
}
