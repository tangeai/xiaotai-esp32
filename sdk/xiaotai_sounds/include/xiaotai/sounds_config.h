// SPDX-License-Identifier: MIT
#pragma once
/* Build-time options of the sound resources (xiaotai/config.h explains the
 * mechanism; the options themselves are in the component's Kconfig).
 *
 * One option per pack under packs/. A pack that is on is embedded in the
 * firmware and registered by xiaotai_engine_sounds_init, in the order below,
 * so a later pack overrides an earlier one name by name. A new pack is one
 * more directory, one more option here and in Kconfig, and one more line in
 * the registration table; the interface in xiaotai/sounds.h does not change. */
#include "xiaotai/config.h"

/* "tones_pcm8k": rings and wake acknowledgements as 8 kHz PCM WAV files. For
 * boards with external RAM and a 16 kHz output. */
#define XIAOTAI_SOUNDS_PACK_TONES_PCM8K XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_SOUNDS_PACK_TONES_PCM8K, 0)
/* "guide_zh_alaw8k": spoken guidance and digits, 8 kHz A-law, 20 ms aligned.
 * For boards whose only codec is G.711. */
#define XIAOTAI_SOUNDS_PACK_GUIDE_ZH_ALAW8K XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_SOUNDS_PACK_GUIDE_ZH_ALAW8K, 0)
