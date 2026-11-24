// SPDX-License-Identifier: MIT
/* Built-in packs: the ones the build embeds (xiaotai/sounds_config.h), in the
 * order they are registered, so a later pack overrides an earlier one name by
 * name. Each pack is the constant xiaotai_sound_pack_<pack> that the pack
 * generator writes from packs/<pack>/pack.json into the build directory; an
 * option that is off references nothing. A new pack is one more block here. */
#include <stddef.h>
#include "xiaotai/sounds.h"
#include "xiaotai/sounds_config.h"

#if XIAOTAI_SOUNDS_PACK_TONES_PCM8K
extern const xiaotai_sound_pack_t xiaotai_sound_pack_tones_pcm8k;
#endif
#if XIAOTAI_SOUNDS_PACK_GUIDE_ZH_ALAW8K
extern const xiaotai_sound_pack_t xiaotai_sound_pack_guide_zh_alaw8k;
#endif

/* NULL-terminated, so a build without packs still has a valid array. */
static const xiaotai_sound_pack_t *const s_builtin[] = {
#if XIAOTAI_SOUNDS_PACK_TONES_PCM8K
    &xiaotai_sound_pack_tones_pcm8k,
#endif
#if XIAOTAI_SOUNDS_PACK_GUIDE_ZH_ALAW8K
    &xiaotai_sound_pack_guide_zh_alaw8k,
#endif
    NULL,
};

/* Runs before every application init, so an application's own pack registered
 * there overrides the built-in sounds. Stops at the first pack refused; the
 * generator has checked every pack at build time, so a refusal is a build
 * defect and fails the start. */
xiaotai_err_t xiaotai_engine_sounds_init(void)
{
    for (size_t i = 0; s_builtin[i]; ++i) {
        xiaotai_err_t err = xiaotai_sounds_register(s_builtin[i]);
        if (err != XIAOTAI_OK) return err;
    }
    return XIAOTAI_OK;
}
