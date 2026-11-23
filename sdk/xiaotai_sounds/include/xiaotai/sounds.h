// SPDX-License-Identifier: MIT
#pragma once
/* Sound resources: prompts, rings and voice guidance, found by name.
 *
 * A sound is immutable data plus a description of its format. Sounds come in
 * packs; a product's build embeds the packs its configuration lists, in the
 * format its audio path can play (PCM for boards with spare memory, A-law or
 * Opus for small ones). Applications, modules and the feedback engine name a
 * sound ("ring.incoming"); which data answers depends on the packs present, so
 * the same application runs on every board. The media engine plays sounds
 * (xiaotai_media_play); this component only holds and describes them.
 *
 * A new sound is one more entry in a pack, a new voice or format one more pack;
 * neither changes this interface. An application may register a pack of its
 * own at init.
 *
 * Tags: [init] during init; [any] any task, once init is over (the index is
 * read-only from then on). */
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/codec.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Largest sound, built in or borrowed. */
#define XIAOTAI_SOUND_BYTES_MAX (10U * 1024U * 1024U)
/* Longest name, without the terminator. */
#define XIAOTAI_SOUND_NAME_MAX 31U

/* Containers. The encoding inside is an audio value of the device's one codec
 * vocabulary (xiaotai/codec.h) that a registered media decoder plays
 * (xiaotai/media_codec.h): this component describes sounds and never decodes
 * them, so a new encoding is a new decoder and a pack, not a change here. */
typedef enum {
    /* Samples back to back, no header: XIAOTAI_CODEC_PCM16 little-endian, or
     * an encoding of one byte per sample (XIAOTAI_CODEC_G711A, _G711U). */
    XIAOTAI_SOUND_RAW = 0,
    /* A complete RIFF/WAVE file: PCM16, mono or stereo, 8000..48000 Hz; the
     * entry's encoding is XIAOTAI_CODEC_PCM16. Rate and channels come from the
     * file. */
    XIAOTAI_SOUND_WAV,
    /* Packets of a framed encoding (Opus), each preceded by its length
     * (1..512) as two bytes, little-endian. */
    XIAOTAI_SOUND_PACKETS,
} xiaotai_sound_container_t;

typedef struct xiaotai_sound {
    const char *name;                    /* lower case, dot separated: "ring.incoming" */
    /* An audio codec, always written out: NONE (a zero-filled entry) is no
     * encoding, not PCM16. */
    xiaotai_codec_t encoding;
    xiaotai_sound_container_t container;
    uint32_t rate_hz;                    /* 0 for XIAOTAI_SOUND_WAV */
    uint8_t channels;                    /* 0 for XIAOTAI_SOUND_WAV */
    uint8_t packet_ms;                   /* XIAOTAI_SOUND_PACKETS: audio per packet; otherwise 0 */
    const uint8_t *data;                 /* immutable while the sound is registered or being played */
    size_t size;
} xiaotai_sound_t;

typedef struct xiaotai_sound_pack {
    const char *name;                    /* "tones_pcm8k" */
    const xiaotai_sound_t *sounds;
    size_t count;
} xiaotai_sound_pack_t;

/* What xiaotai_sounds_inspect reads from the data. */
typedef struct {
    uint32_t rate_hz;
    uint8_t channels;
    uint32_t frames;        /* sample frames (RAW, WAV) or packets (PACKETS) */
    uint32_t duration_ms;   /* rounded up */
    /* The audio inside the container: the samples of a WAV file (its "data"
     * chunk), everything for RAW and PACKETS. Points into the sound's data and
     * lives as long as that does. */
    const uint8_t *payload;
    size_t payload_size;
} xiaotai_sound_info_t;

/* Names every pack uses for the same purpose. A pack carries the subset its
 * product needs; a missing name is XIAOTAI_ERR_NOT_FOUND at play time and the
 * caller decides what to do without it. */
#define XIAOTAI_SOUND_RING_INCOMING "ring.incoming"
#define XIAOTAI_SOUND_RING_OUTGOING "ring.outgoing"
#define XIAOTAI_SOUND_ACK_FEMALE "ack.female"            /* answer to the wake word */
#define XIAOTAI_SOUND_ACK_MALE "ack.male"
#define XIAOTAI_SOUND_CUE_TAP "cue.tap"
#define XIAOTAI_SOUND_CUE_SUCCESS "cue.success"
#define XIAOTAI_SOUND_CUE_WARNING "cue.warning"
#define XIAOTAI_SOUND_CUE_ENDED "cue.ended"
#define XIAOTAI_SOUND_GUIDE_STARTUP "guide.startup"
#define XIAOTAI_SOUND_GUIDE_WIFI_SETUP "guide.wifi_setup"
#define XIAOTAI_SOUND_GUIDE_WIFI_CONNECTING "guide.wifi_connecting"
#define XIAOTAI_SOUND_GUIDE_WIFI_CONNECTED "guide.wifi_connected"
#define XIAOTAI_SOUND_GUIDE_WIFI_FAILED "guide.wifi_failed"
#define XIAOTAI_SOUND_GUIDE_BINDING "guide.binding"      /* ends right before the spoken code */
#define XIAOTAI_SOUND_GUIDE_BOUND "guide.bound"
#define XIAOTAI_SOUND_GUIDE_OFFLINE "guide.offline"
#define XIAOTAI_SOUND_GUIDE_ONLINE "guide.online"
#define XIAOTAI_SOUND_GUIDE_INCOMING "guide.incoming"
#define XIAOTAI_SOUND_GUIDE_DIALING "guide.dialing"
#define XIAOTAI_SOUND_GUIDE_ENDED "guide.ended"
#define XIAOTAI_SOUND_GUIDE_FAILED "guide.failed"

/* [init] Adds a pack; the pack, its table, names and data must stay valid for
 * good. A later pack overrides an earlier one name by name, so an application
 * can replace a built-in sound. XIAOTAI_ERR_INVALID_ARG: an entry without
 * name, data or size, an encoding that is not an audio codec
 * (XIAOTAI_CODEC_IS_AUDIO), a name longer than XIAOTAI_SOUND_NAME_MAX or a
 * size above XIAOTAI_SOUND_BYTES_MAX. XIAOTAI_ERR_NO_MEM: too many packs. */
xiaotai_err_t xiaotai_sounds_register(const xiaotai_sound_pack_t *pack);

/* [any] NULL when no pack has the name. The result stays valid for good. */
const xiaotai_sound_t *xiaotai_sounds_find(const char *name);

/* [any] Name of the spoken digit 0..9 ("digit.0" ...), for codes read out
 * digit by digit; NULL beyond 9. The string has static lifetime. */
const char *xiaotai_sounds_digit(unsigned digit);

/* [any] Checks the container without playing and fills info. Works on built-in
 * and on borrowed sounds (a downloaded file described by a caller's
 * xiaotai_sound_t). XIAOTAI_ERR_INVALID_ARG: NULL or empty;
 * XIAOTAI_ERR_INVALID_SIZE: above XIAOTAI_SOUND_BYTES_MAX, a truncated file or
 * packet, a length that is not whole frames; XIAOTAI_ERR_NOT_SUPPORTED: a WAV
 * that is not PCM16 mono or stereo at 8000..48000 Hz, or a malformed one. */
xiaotai_err_t xiaotai_sounds_inspect(const xiaotai_sound_t *sound, xiaotai_sound_info_t *info);

/* [any] Every distinct name, for listings and diagnostics; index past the end
 * returns NULL. */
size_t xiaotai_sounds_count(void);
const xiaotai_sound_t *xiaotai_sounds_at(size_t index);

/* Engine lifecycle (called by the framework through the generated tables):
 * registers the packs the build embedded. */
xiaotai_err_t xiaotai_engine_sounds_init(void);
/* Host tests: forget every pack. */
void xiaotai_sounds_reset(void);

#ifdef __cplusplus
}
#endif
