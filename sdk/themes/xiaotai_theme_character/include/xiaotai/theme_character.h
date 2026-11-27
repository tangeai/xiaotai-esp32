// SPDX-License-Identifier: MIT
#pragma once
/* The on-screen character: an anime-style figure who stands on the HOME and
 * TALK pages of large colour displays in place of the kit's small face. She
 * listens, speaks with her mouth following what is heard from the speaker,
 * shows the emotions of a conversation in her face, reacts to what the device
 * is doing (network, calls, an alarm, the volume, errors) and to touch (a
 * poke, a stroke over the head, a hand held), and keeps herself busy when
 * nothing happens. Who she is comes from the sprite pack: a figure in one
 * piece (the shipped one) or a layered one, with or without animal ears, a
 * tail, arms, a head that turns.
 *
 * This unit ("theme_character") is an actor of the colour theme kit
 * (xiaotai/theme_lcd.h): selecting it in a product registers the actor, and
 * the kit uses it on the displays it matches. Products that do not select it
 * are built without it and keep the kit's face.
 *
 * Match: landscape RGB565 displays at least 800 pixels wide and 560 to 720
 * pixels high (the sprites are baked for a page about 600 pixels high;
 * tools/character/bake.py bakes another size). The manifest asks for external
 * RAM: the pictures of the base figure and the places for her poses live there.
 *
 * Inputs
 * - the view (xiaotai/view.h): emotion (every name the kit accepts), phase and
 *   level, as for the face;
 * - sound: the loudness the media engine publishes (xiaotai_media_loudness),
 *   when the product has the media engine;
 * - events, by name, from whichever of these units the product has:
 *   system.started, net.up, net.down, platform.online, platform.offline,
 *   call.incoming, call.connected, call.ended, alarm.ringing, ai_talk.state
 *   (a talk being connected, and its last words), settings.changed
 *   (audio.volume, audio.mic_muted), settings.refused, input.rejected,
 *   media.output_error;
 * - the pointer of the display, read every frame.
 *
 * Test bench: AT+CHARACTER (state), AT+CHARACTER CUE <name> [<value>] (play a
 * reaction), AT+CHARACTER SHOW <mood> [<phase> [<conditions> [<seconds>]]]
 * (hold a mood, a phase 0..3 and condition bits for a while). The actor's
 * counters are part of AT+UI PERF.
 *
 * The artwork of the shipped pack is the project's own (THIRD_PARTY.md). */
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

struct xiaotai_theme_lcd_actor;
extern const struct xiaotai_theme_lcd_actor xiaotai_theme_character_actor;

/* Lifecycle unit "theme_character" (theme.json): registers the actor with the
 * kit, subscribes to the events above and registers AT+CHARACTER. */
xiaotai_err_t xiaotai_engine_theme_character_init(void);

#ifdef __cplusplus
}
#endif
