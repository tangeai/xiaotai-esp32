#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the ESP I2S lifetime helpers against a READY/RUNNING IDF model."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


MODEL = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef int esp_err_t;
typedef esp_err_t xiaotai_err_t;
enum {ESP_OK, ESP_ERR_INVALID_STATE, INFO_FAIL, PRELOAD_FAIL, RX_FAIL, TX_FAIL};
typedef struct {
    bool is_enabled;
    size_t total_dma_buf_size;
} i2s_chan_info_t;

typedef struct {
    bool enabled, tx;
    size_t dma_bytes, loaded, preloads;
    esp_err_t info_error, disable_error, enable_error;
    size_t fail_at, short_at;
    uint8_t data[4 * 160 * 2 * sizeof(int16_t)];
} model_channel_t, *i2s_chan_handle_t;
typedef struct xt_audio_io { i2s_chan_handle_t rx, tx; } xt_audio_io_t;
static char order[128];
static size_t used;

static void mark(char operation)
{
    assert(used + 1 < sizeof(order));
    order[used++] = operation;
    order[used] = 0;
}

static void clear_order(void) { order[used = 0] = 0; }

static esp_err_t i2s_channel_get_info(i2s_chan_handle_t channel, i2s_chan_info_t *info)
{
    if (channel->info_error) return channel->info_error;
    *info = (i2s_chan_info_t){channel->enabled, channel->dma_bytes};
    return ESP_OK;
}

static esp_err_t i2s_channel_enable(i2s_chan_handle_t channel)
{
    assert(!channel->enabled);
    mark(channel->tx ? 'T' : 'R');
    if (channel->enable_error) return channel->enable_error;
    if (channel->tx) assert(channel->loaded == channel->dma_bytes);
    channel->enabled = true;
    return ESP_OK;
}

static esp_err_t i2s_channel_disable(i2s_chan_handle_t channel)
{
    assert(channel->enabled);
    mark(channel->tx ? 't' : 'r');
    if (channel->disable_error) return channel->disable_error;
    channel->enabled = false;
    channel->loaded = 0; /* The IDF resets its cursor, not payload bytes. */
    channel->preloads = 0;
    return ESP_OK;
}

static esp_err_t i2s_channel_preload_data(i2s_chan_handle_t channel, const void *src,
                                          size_t bytes, size_t *loaded)
{
    assert(channel->tx && !channel->enabled);
    assert(bytes && bytes <= channel->dma_bytes - channel->loaded);
    for (size_t i = 0; i < bytes; ++i) assert(((const uint8_t *)src)[i] == 0);
    mark('p');
    if (++channel->preloads == channel->fail_at) return PRELOAD_FAIL;
    if (channel->preloads == channel->short_at) --bytes;
    memcpy(channel->data + channel->loaded, src, bytes);
    channel->loaded += bytes;
    *loaded = bytes;
    return ESP_OK;
}
"""

CASES = r"""
int main(void)
{
    model_channel_t rx = {.enabled = true};
    model_channel_t tx = {.enabled = true, .tx = true, .dma_bytes = sizeof(tx.data)};
    const xt_audio_io_t channels = {.rx = &rx, .tx = &tx};
    xt_audio_io_t io = channels;
    memset(tx.data, 0xa5, sizeof(tx.data));

    assert(xt_audio_io_pause(&io) == ESP_OK);
    assert(!strcmp(order, "rt") && !rx.enabled && !tx.enabled);
    assert(tx.data[0] == 0xa5 && tx.data[sizeof(tx.data) - 1] == 0xa5);
    assert(xt_audio_io_pause(&io) == ESP_OK && !strcmp(order, "rt"));
    assert(xt_audio_io_resume(&io) == ESP_OK);
    assert(tx.loaded == sizeof(tx.data) && rx.enabled && tx.enabled);
    assert(used > 4 && order[used - 2] == 'T' && order[used - 1] == 'R');
    for (size_t i = 2; i < used - 2; ++i) assert(order[i] == 'p');
    for (size_t i = 0; i < sizeof(tx.data); ++i) assert(tx.data[i] == 0);
    assert(io.rx == channels.rx && io.tx == channels.tx);

    clear_order();
    assert(xt_audio_io_resume(&io) == ESP_ERR_INVALID_STATE && !used);
    rx.disable_error = RX_FAIL;
    tx.disable_error = TX_FAIL;
    assert(xt_audio_io_pause(&io) == RX_FAIL && !strcmp(order, "rt"));
    assert(rx.enabled && tx.enabled);
    rx.disable_error = tx.disable_error = ESP_OK;
    assert(xt_audio_io_pause(&io) == ESP_OK);

    clear_order();
    tx.short_at = 2;
    assert(xt_audio_io_resume(&io) == ESP_ERR_INVALID_STATE && !strcmp(order, "pp"));
    assert(!rx.enabled && !tx.enabled && io.tx == channels.tx && io.rx == channels.rx);
    clear_order();
    tx.short_at = 0;
    tx.loaded = tx.preloads = 0;
    tx.fail_at = 2;
    assert(xt_audio_io_resume(&io) == PRELOAD_FAIL && !strcmp(order, "pp"));
    assert(!rx.enabled && !tx.enabled);

    clear_order();
    tx.fail_at = 0;
    tx.loaded = tx.preloads = 0;
    tx.info_error = INFO_FAIL;
    assert(xt_audio_io_resume(&io) == INFO_FAIL && !used);
    tx.info_error = ESP_OK;
    tx.dma_bytes = 0;
    assert(xt_audio_io_resume(&io) == ESP_ERR_INVALID_STATE && !used);

    tx.dma_bytes = 157; /* Latest IDF geometry, including a short final chunk. */
    tx.enable_error = TX_FAIL;
    assert(xt_audio_io_resume(&io) == TX_FAIL);
    assert(tx.loaded == 157 && order[used - 1] == 'T' && !rx.enabled && !tx.enabled);

    clear_order();
    tx.enable_error = ESP_OK;
    tx.loaded = tx.preloads = 0;
    rx.enable_error = RX_FAIL;
    assert(xt_audio_io_resume(&io) == RX_FAIL);
    assert(used > 2 && order[used - 2] == 'T' && order[used - 1] == 'R');
    assert(!rx.enabled && tx.enabled);
    assert(xt_audio_io_pause(&io) == ESP_OK && order[used - 1] == 't');
    assert(!tx.enabled && !rx.enabled && io.tx == &tx && io.rx == &rx);

    clear_order();
    rx.enable_error = ESP_OK;
    assert(xt_audio_io_resume(&(xt_audio_io_t){.rx = &rx}) == ESP_OK);
    assert(!strcmp(order, "R"));
    return 0;
}
"""


def definition(source, name):
    result = re.search(r"^[\w ]+\b" + re.escape(name) + r"\([^;{}]*\)\n\{[\s\S]*?^\}", source, re.MULTILINE)
    if result is None:
        raise ValueError(f"Missing native definition: {name}")
    return result[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    port = Path(__file__).resolve().parents[1] / "esp-idf" / "audio_i2s.c"
    source = port.read_text(encoding="utf-8")
    helpers = [definition(source, name) for name in
               ("enable", "xt_audio_io_pause", "clear_tx", "xt_audio_io_resume")]
    with tempfile.TemporaryDirectory(prefix="xt-audio-dma-lifetime-") as scratch:
        fixture = Path(scratch) / "lifetime.c"
        binary = Path(scratch) / "lifetime"
        fixture.write_text("\n".join([MODEL, *helpers, CASES]), encoding="utf-8")
        subprocess.run([args.cc, "-std=c17", "-Wall", "-Wextra", "-Werror", "-UNDEBUG",
                        "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                        fixture, "-o", binary], check=True)
        subprocess.run([binary], check=True)
    print("PASS: native I2S pause/resume retains handles, clears TX in READY, and preserves failures")


if __name__ == "__main__":
    main()
