// SPDX-License-Identifier: MIT
#pragma once
/* Build-time options of the media engine (xiaotai/config.h explains the
 * mechanism; the options themselves are in the component's Kconfig).
 *
 * Each option adds one implementation that wraps a platform library to the
 * build; the implementation registers itself with the engine's registries
 * (xiaotai/media_capture.h, xiaotai/media_codec.h) during init. What then runs
 * on a board is still chosen at start, from the part descriptors and the
 * declared formats: an option that is on but matches nothing costs flash, not
 * RAM. The portable implementations (G.711, echo gate, pass-through,
 * high-pass, noise gate) are always built and have no option.
 *
 * All default to off, on every platform: the product configuration turns on
 * what the product's audio parts need, and host tests register fakes. The one
 * exception is a policy, not an implementation: the front-end memory option
 * below follows external RAM.
 *
 * The last three options compile console commands instead (frame trace, audio
 * test commands, sound previews). Each lives in its own source file whose
 * handler names share the prefix the manifest's commands give as "symbol", so
 * the post-link check can prove that an option that is off left no handler in
 * the image. */
#include "xiaotai/config.h"

#define XIAOTAI_MEDIA_ALAW_FIXED_FRAMES XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_MEDIA_ALAW_FIXED_FRAMES, 0)

/* Processor "aec.dual": two microphones and a reference, with speech
 * enhancement. */
#define XIAOTAI_MEDIA_PROCESSOR_AEC_DUAL XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_MEDIA_PROCESSOR_AEC_DUAL, 0)
/* Processor "aec.single.frontend": one microphone and a reference, through the
 * speech front end (its own worker task). */
#define XIAOTAI_MEDIA_PROCESSOR_AEC_SINGLE_FRONTEND \
    XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_MEDIA_PROCESSOR_AEC_SINGLE_FRONTEND, 0)
/* Processor "aec.single": one microphone and a reference, the canceller called
 * directly on the capture task. */
#define XIAOTAI_MEDIA_PROCESSOR_AEC_SINGLE XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_MEDIA_PROCESSOR_AEC_SINGLE, 0)
/* Speech front-end memory policy of "aec.dual" and "aec.single.frontend":
 * 1 keeps the working set in external RAM. Kconfig defaults it on wherever
 * external RAM exists and the symbol is absent elsewhere (0); the host has no
 * front end and uses 0. */
#define XIAOTAI_MEDIA_AFE_MEMORY_EXTERNAL XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_MEDIA_AFE_MEMORY_EXTERNAL, 0)
/* Effects "agc" (capture stage) and "boost" (uplink stage): automatic gain
 * control and limiter. */
#define XIAOTAI_MEDIA_EFFECT_AGC XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_MEDIA_EFFECT_AGC, 0)
/* Codec "opus": decoder and encoder are separate options because an encoded
 * input needs only the decoder. */
#define XIAOTAI_MEDIA_CODEC_OPUS_DECODER XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_MEDIA_CODEC_OPUS_DECODER, 0)
#define XIAOTAI_MEDIA_CODEC_OPUS_ENCODER XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_MEDIA_CODEC_OPUS_ENCODER, 0)
/* Frame trace (checksums of sent and received packets for comparing two
 * devices) and its diagnostic commands. */
#define XIAOTAI_MEDIA_TRACE XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_MEDIA_TRACE, 0)
/* Optional metadata-only automatic capture; separate from on-demand trace. */
#define XIAOTAI_MEDIA_TRACE_ARRIVALS XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_MEDIA_TRACE_ARRIVALS, 0)
/* Console commands that act on the audio parts: AT+AUDIO TONE, RECORD, PLAY,
 * STOP, DUMP, SENDTONE (local tone, one-second microphone recording and its
 * export, uplink test tone). Off by default because RECORD and DUMP expose
 * the microphone on the console; products keep them only where their current
 * firmware has them. The engine's record and tone functions in
 * xiaotai/media.h exist either way. */
#define XIAOTAI_MEDIA_TEST_COMMANDS XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_MEDIA_TEST_COMMANDS, 0)
/* Console commands that list and play registered sounds: AT+SOUND, AT+SOUND
 * PLAY, AT+SOUND STOP. */
#define XIAOTAI_MEDIA_SOUND_COMMANDS XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_MEDIA_SOUND_COMMANDS, 0)
