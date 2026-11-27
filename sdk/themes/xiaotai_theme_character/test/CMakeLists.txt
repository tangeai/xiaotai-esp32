# SPDX-License-Identifier: MIT
# Host tests of the on-screen character.
#
# theme_character_core: the character without a display (pack checks, shapes
# and sprites inside their clip, moods, conversation, conditions, touch,
# springs, damage). chr_gallery.c is not a test: tools/character_preview.py
# compiles it to draw the pictures.
#
# theme_character_director: what each event of the device does. That the event
# names are the ones the emitting units publish is checked by
# tests/sdk/integration (theme_character_events), where those units may be seen.
#
# theme_character_shelf: the pack behind an image on a slow medium (src/chr_shelf.c).
#
# theme_character_actor: the character on a page as the kit drives it (src/chr_actor.c), without drawing.
xiaotai_host_test(theme_character_core SOURCES character_core_test.c LINK xiaotai_theme_character m INCLUDE ../src ../host)
xiaotai_host_test(theme_character_shelf SOURCES character_shelf_test.c LINK xiaotai_theme_character m INCLUDE ../src)
xiaotai_host_test(theme_character_director SOURCES character_director_test.c LINK xiaotai_theme_character
                  INCLUDE ../src ../host)
xiaotai_host_test(theme_character_actor SOURCES character_actor_test.c LINK xiaotai_theme_character lvgl::lvgl
                  INCLUDE ../src ROLES lvgl)
if(TARGET theme_character_actor)
    target_link_options(theme_character_actor PRIVATE
        "LINKER:--wrap=xiaotai_time_us" "LINKER:--wrap=xiaotai_ui_host_display" "LINKER:--wrap=xiaotai_ui_host_root"
        "LINKER:--wrap=xiaotai_ui_host_painter" "LINKER:--wrap=chr_inspect" "LINKER:--wrap=chr_return")
endif()
