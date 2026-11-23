// SPDX-License-Identifier: MIT
#pragma once
#include "xiaotai/parts/audio.h"

typedef struct xt_audio_io xt_audio_io_t;
typedef void (*xt_audio_overflow_fn)(void *ctx);
typedef void (*xt_audio_rx_block_fn)(void *ctx, const void *data, size_t size);

/* Pure capability check and resolution of platform default DMA geometry.
 * Ignore wire fields for a direction that is not configured. */
xiaotai_err_t xt_audio_io_describe(xiaotai_i2s_config_t *i2s, uint8_t rx_channels, uint8_t tx_channels);

/* Acquire/start all requested channels. On failure *io is NULL only if the
 * backend left nothing to close; otherwise it identifies the partial device.
 * Either RX callback is called in the receive ISR, never both. Receive sees
 * one native 16-bit DMA block in wire slot order, borrowed only for the call.
 * The backend serializes calls for the same port. */
xiaotai_err_t xt_audio_io_start(const xiaotai_i2s_config_t *i2s, uint8_t rx_channels, uint8_t tx_channels,
                                xt_audio_overflow_fn overflow, xt_audio_rx_block_fn receive,
                                void *ctx, xt_audio_io_t **io);
/* Nonzero identity of the calling task, used only for comparison. */
uintptr_t xt_audio_io_reader_id(void);
/* Stop or resume configured channels without releasing DMA. Resume clears
 * old TX data before clocks can expose it to the codec. */
xiaotai_err_t xt_audio_io_pause(xt_audio_io_t *io);
xiaotai_err_t xt_audio_io_resume(xt_audio_io_t *io);
/* On success no callbacks remain and the handle is no longer valid. On
 * failure the backend keeps its handles and callback storage alive. */
xiaotai_err_t xt_audio_io_stop(xt_audio_io_t *io);
/* Drop queued capture when its part closes/rejoins a running playback group.
 * Leave the shared clock and playback alone. */
xiaotai_err_t xt_audio_io_discard_input(xt_audio_io_t *io);

/* Whole logical frames, contiguous in descriptor order. The port maps wire
 * slots (including mono playback duplicated into two slots), honors buffer
 * placement, and has no allocation or logging on these paths. */
xiaotai_err_t xt_audio_io_read(xt_audio_io_t *io, void *data, size_t capacity, size_t *size);
xiaotai_err_t xt_audio_io_write(xt_audio_io_t *io, const void *data, size_t size, size_t *written);
void xt_audio_io_report(const xiaotai_i2s_config_t *i2s, bool running, xiaotai_hal_write_fn_t write, void *ctx);
