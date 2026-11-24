// SPDX-License-Identifier: MIT
#pragma once
/* Parts library: PCM audio on an I2S port - one audio device that gives the
 * board an audio input part, an audio output part, or both.
 *
 * An audio device is one I2S port with its codec chips: capture and playback
 * share the bit clock and the frame clock, so they are one group in the sense
 * of xiaotai/hal.h (opening either part starts the port; it stops when both
 * are closed). The chips are control objects created here (ES8311, ES8389,
 * ES7210, AW88298); a new chip is one more create function and one more file in this
 * component, and nothing else changes. A direction may also have no control
 * chip at all (an I2S or PDM MEMS microphone, a class-D amplifier with a
 * digital input): the port alone carries it.
 *
 * What the device owns, so that no board repeats it: the start order of port
 * and chips and the complete rollback of a failed start, amplifier timing,
 * the mute rule when the amplifier has no enable line, volume and gain
 * bookkeeping (a value set while closed takes effect at the next open), the
 * capture ring and its single reader, flush_frames from the DMA geometry, and
 * the counters behind stats().
 *
 * Failure rules (xiaotai/hal.h): an open() that fails is rolled back and may
 * be retried. When the rollback fails, or when the device runs its capture
 * ring (the interrupt path cannot be torn down safely), the device latches:
 * every later open() returns XIAOTAI_ERR_INVALID_STATE until restart.
 *
 * Calling tasks: create functions are [board] (the board's Init()). The parts
 * follow xiaotai/hal.h: open/close/set_* are [engine], read/write are [rt]
 * and neither allocate nor log, stats is [any]. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/hal.h"
#include "xiaotai/parts/bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- codec chips */

/* The control side of a codec or amplifier chip. Opaque; allocated once at
 * [board] (XIAOTAI_MEM_INTERNAL) and never freed. A chip object serves one
 * audio device. */
typedef struct xiaotai_audio_codec xiaotai_audio_codec_t;

/* ES8311: mono codec (ADC and DAC). */
typedef struct {
    xiaotai_i2c_bus_t *bus;
    uint8_t address;            /* 7-bit; 0: the chip's default */
    uint16_t mclk_multiple;     /* master clock / sample rate; 0: the chip runs from the bit clock */
    bool capture, playback;     /* directions the board uses */
    /* Capture delivers two channels: the microphone, then a loopback of what
     * the DAC plays (roles MIC, REFERENCE). */
    bool reference;
    /* Supplies of the amplifier and of the DAC in millivolts: they set the
     * hardware gain the volume law compensates. 0: 5000 and 3300. */
    uint16_t amplifier_mv, dac_mv;
    /* Device transfer clock in Hz on the same bus. 0 uses the bus default. */
    uint32_t i2c_hz;
    /* Preserve ADC-left/DAC-right codec routing on boards that deliver only
     * the microphone slot. Does not add a REFERENCE channel to the HAL. */
    bool route_dac_to_unused_slot;
} xiaotai_es8311_config_t;

xiaotai_err_t xiaotai_es8311_create(const xiaotai_es8311_config_t *config, xiaotai_audio_codec_t **codec);

/* ES8389: codec with two ADC inputs and a DAC. Capture delivers MIC1 and MIC2;
 * with reference, the chip adds a loopback of what the DAC plays and the port
 * carries four time-division slots: MIC1, the loopback, MIC2, an unused slot
 * (roles MIC, REFERENCE, MIC, UNUSED). */
typedef struct {
    xiaotai_i2c_bus_t *bus;
    uint8_t address;            /* 7-bit; 0: the chip's default */
    uint16_t mclk_multiple;     /* master clock / sample rate; 0: the chip runs from the bit clock */
    bool capture, playback;     /* directions the board uses */
    bool reference;
    /* bit n: ADC input n (MIC1 is bit 0) feeds a microphone; 0 with capture: both. */
    uint8_t inputs;
    /* Supplies of the amplifier and of the DAC in millivolts: they set the
     * hardware gain the volume law compensates. 0: 5000 and 3300. */
    uint16_t amplifier_mv, dac_mv;
    /* Device transfer clock in Hz on the same bus. 0 uses the bus default. */
    uint32_t i2c_hz;
} xiaotai_es8389_config_t;

xiaotai_err_t xiaotai_es8389_create(const xiaotai_es8389_config_t *config, xiaotai_audio_codec_t **codec);

/* ES7210: four-input ADC, time-division output. */
typedef struct {
    xiaotai_i2c_bus_t *bus;
    uint8_t address;            /* 7-bit; 0: the chip's default */
    uint16_t mclk_multiple;
    uint8_t inputs;             /* bit n: input n (MIC1 is bit 0) is converted */
} xiaotai_es7210_config_t;

xiaotai_err_t xiaotai_es7210_create(const xiaotai_es7210_config_t *config, xiaotai_audio_codec_t **codec);

/* AW88298: digital-input amplifier. It has one mute bit and no enable line;
 * the device mutes it while the output is disabled. */
typedef struct {
    xiaotai_i2c_bus_t *bus;
    uint8_t address;            /* 7-bit; 0: the chip's default */
    xiaotai_line_t reset;       /* zeroed: not connected */
    /* Bit clocks per frame on the shared port (64 when four 16-bit capture
     * slots share the clock with playback). The chip is programmed for it
     * after both directions run and the setting is read back; a mismatch fails
     * the open with XIAOTAI_FAIL. 0: the chip's default. */
    uint8_t bit_clocks_per_frame;
} xiaotai_aw88298_config_t;

xiaotai_err_t xiaotai_aw88298_create(const xiaotai_aw88298_config_t *config, xiaotai_audio_codec_t **codec);

/* ------------------------------------------------------------ audio device */

/* Wire format of one direction of the port. */
typedef enum {
    /* Slots in frames: 2 slots is standard (Philips) I2S, more is TDM. */
    XIAOTAI_I2S_SLOTS = 0,
    /* PDM: ws_pin carries the PDM clock, bclk_pin is unused (-1). Receive
     * delivers PCM (the port decimates), rx_slots 1 or 2; transmit takes PCM
     * and modulates it. A platform that cannot run the requested combination
     * on this port fails create with XIAOTAI_ERR_NOT_SUPPORTED. */
    XIAOTAI_I2S_PDM,
} xiaotai_i2s_mode_t;

typedef struct {
    int port;                   /* I2S controller index of the chip */
    int mclk_pin;               /* -1: no master clock output */
    int bclk_pin, ws_pin;
    int din_pin, dout_pin;      /* -1: direction not wired */
    uint32_t rate_hz;
    uint16_t mclk_multiple;
    /* 16-bit slots per frame. Receive: 2 is standard I2S, more is TDM.
     * Transmit: 2 (a mono output part is duplicated into both). */
    uint8_t rx_slots, tx_slots;
    /* DMA geometry of each direction; 0: the platform's default. flush_frames
     * and latency are derived from it. */
    uint8_t dma_descriptors;
    uint16_t dma_frames;
    xiaotai_i2s_mode_t rx_mode, tx_mode;
} xiaotai_i2s_config_t;

typedef enum {
    /* read() takes blocks from the port driver and blocks until one is there.
     * The driver's receive-queue overflow (a DMA block dropped because nobody
     * read in time) is counted from its interrupt callback: one overrun and
     * dma_frames lost frames per event, in stats(). The callback only counts. */
    XIAOTAI_AUDIO_CAPTURE_BLOCKING = 0,
    /* The receive interrupt copies every DMA block into a ring of ring_blocks
     * blocks (XIAOTAI_MEM_BULK). A full ring drops the new block and counts an
     * overrun; a block of unexpected size is dropped and counted as an error.
     * One reader task: a second task's read() returns
     * XIAOTAI_ERR_INVALID_STATE. read() waits up to read_timeout_ms. The
     * interrupt needs the flash cache, so the platform must not make the I2S
     * interrupt cache-safe (the implementation refuses to build otherwise). */
    XIAOTAI_AUDIO_CAPTURE_RING,
} xiaotai_audio_capture_mode_t;

typedef struct {
    xiaotai_i2s_config_t i2s;
    uint8_t group;              /* non-zero; unique among the board's audio devices */

    /* ---- capture; capture_channels 0: the device has no input part */
    /* Control chip of the capture path; NULL: none (the gain is then fixed
     * and input_gain_db, mic_inputs and the gain range are ignored). */
    xiaotai_audio_codec_t *capture_codec;
    uint8_t capture_channels;   /* channels delivered, in wire (slot) order */
    xiaotai_channel_role_t capture_roles[XIAOTAI_AUDIO_CHANNELS_MAX];
    /* Gain set after every start, indexed by the codec's input number (ES7210:
     * MIC1 is 0), which is not the wire slot. */
    int8_t input_gain_db[XIAOTAI_AUDIO_CHANNELS_MAX];
    /* bit n: codec input n feeds a microphone channel (MIC or MONITOR).
     * set_gain_db() changes exactly these; the descriptor's gain is theirs. */
    uint8_t mic_inputs;
    int8_t gain_min_db, gain_max_db;    /* min == max: the input part has no set_gain_db */
    uint8_t gain_step_db;
    int8_t echo_coupling_db;    /* measured by the board, or XIAOTAI_AUDIO_COUPLING_UNKNOWN */
    uint16_t capture_settle_ms; /* start transient the reader should discard */
    xiaotai_audio_capture_mode_t capture_mode;
    uint8_t ring_blocks;        /* RING: blocks in the ring */
    uint16_t read_timeout_ms;   /* RING: bound of one read(); BLOCKING: 0 */

    /* ---- playback; playback_channels 0: the device has no output part */
    /* Control chip of the playback path; NULL: none. Volume is then applied
     * by write() to the samples in place (xiaotai/hal.h allows it), and mute
     * is the amplifier line, or zeroed samples without one. */
    xiaotai_audio_codec_t *playback_codec;
    uint8_t playback_channels;  /* 1 (role MONO) or 2 (LEFT, RIGHT) */
    /* Highest codec volume this board plays at, 1..100 %; 0 means 100. Set it
     * to where the speaker path stops being linear (measured on the board):
     * volume 1..100 % is scaled into 1..volume_max_percent, and the output's
     * volume_span_cdb shrinks in proportion, so 100 % stays the 0 dB point
     * the HAL describes. */
    uint8_t volume_max_percent;
    /* Output info.max_write_frames: the block the board's playout is built
     * around (one DMA descriptor on some boards). 0: any size; the engine
     * then writes 20 ms blocks. */
    uint16_t max_write_frames;
    /* Amplifier enable. zeroed: the amplifier is switched by the codec's mute,
     * which is then "output disabled or muted by the engine". */
    xiaotai_line_t amplifier;
    uint16_t amplifier_enable_ms;   /* set_enabled(true) returns this long after the line rose */
    /* Frames of silence written before the amplifier turns on, so that it
     * never starts on an empty DAC. */
    uint16_t amplifier_pad_frames;

    /* The port and both chips are started and stopped once inside create to
     * prove the format; a failure fails create. */
    bool verify_at_create;
} xiaotai_audio_device_config_t;

/* [board] Creates the device and its parts; apart from verify_at_create it
 * touches no hardware. *input and *output receive the parts, NULL for a
 * direction the configuration leaves out (either pointer may itself be NULL).
 * Objects are allocated once and never freed; the configuration is copied.
 *
 * The parts' descriptors follow from the configuration: encoding
 * XIAOTAI_CODEC_PCM16 (written explicitly: 0 is XIAOTAI_CODEC_NONE) at
 * i2s.rate_hz;
 * output info.flags carries XIAOTAI_AUDIO_OUTPUT_PACED; flush_frames() is
 * (dma_descriptors + 1) * dma_frames while open and 0 while closed;
 * info.latency_ms is dma_descriptors * dma_frames at the sample rate (6 x 240
 * frames at 16 kHz: 90 ms), so it is never left to the engine's fallback;
 * info.max_write_frames is max_write_frames.
 *
 * XIAOTAI_ERR_INVALID_ARG: no direction at all (both channel counts 0), a
 * channel count the slots cannot carry, a group of 0, RING without
 * ring_blocks or read_timeout_ms. */
xiaotai_err_t xiaotai_audio_device_create(const xiaotai_audio_device_config_t *config,
                                          const xiaotai_audio_input_t **input,
                                          const xiaotai_audio_output_t **output);

/* Counters of one device beyond xiaotai_audio_stats_t, for
 * xiaotai_hal_report() and diagnostics. */
typedef struct {
    uint32_t opens, open_failures;
    uint32_t ring_overflows;        /* RING: blocks dropped because the ring was full */
    uint32_t receive_overflows;     /* BLOCKING: blocks the driver's receive queue dropped */
    uint32_t bad_blocks;            /* RING: DMA blocks of unexpected size */
    uint32_t read_timeouts;
    uint32_t write_errors;
    uint16_t peak[XIAOTAI_AUDIO_CHANNELS_MAX];      /* largest magnitude read per channel since the last call */
    uint32_t nonzero[XIAOTAI_AUDIO_CHANNELS_MAX];   /* non-zero samples read per channel since boot */
    bool running;                   /* the port is started */
    bool latched;                   /* the device refuses to open until restart */
    xiaotai_err_t last_error;
} xiaotai_audio_device_stats_t;

/* [any] Never blocks. part is the device's input or output part. Reading
 * resets the peaks. */
void xiaotai_audio_device_stats(const void *part, xiaotai_audio_device_stats_t *stats);

/* [any] Diagnostics of the device for the board's Report(): codec registers
 * and, when the port is stopped, the activity seen on its pins. Blocks for bus
 * transfers; never from a real-time task. */
void xiaotai_audio_device_report(const void *part, xiaotai_hal_write_fn_t write, void *ctx);

#ifdef __cplusplus
}
#endif
