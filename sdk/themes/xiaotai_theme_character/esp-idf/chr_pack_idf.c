// SPDX-License-Identifier: MIT
/* The sprite pack where the build puts it: behind the application image, in the application's own
 * partition (scripts/image_trailer.py). It is read through the flash driver, in pieces, by the
 * shelf's task (src/chr_shelf.c says why it is neither linked in nor mapped). */
#include <stdio.h>
#include <string.h>

#include "chr_director.h"
#include "chr_shelf.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

enum { PIECE = 4096, WAIT_MS = 1500 };

static chr_shelves_t *shelves;
static const esp_partition_t *app;

/* The driver can only read straight into internal RAM; given anything else it allocates a buffer
 * there for every call. This one is always there, and only the shelf's task uses it. */
static uint8_t piece[PIECE] __attribute__((aligned(4)));

static uint32_t open_partition(void *ctx)
{
    (void)ctx;
    app = esp_ota_get_running_partition();
    return app ? app->size : 0;
}

static bool read_partition(void *ctx, uint32_t at, void *to, size_t bytes)
{
    (void)ctx;
    uint8_t *out = to;
    while (bytes) {
        const size_t some = bytes < PIECE ? bytes : PIECE;
        if (esp_partition_read(app, at, piece, some) != ESP_OK) return false;
        memcpy(out, piece, some);
        at += (uint32_t)some;
        out += some;
        bytes -= some;
    }
    return true;
}

const struct chr_pack *chr_pack_carried(void)
{
    if (!shelves) {
        const chr_medium_t medium = {.open = open_partition, .read = read_partition};
        shelves = chr_shelves_open(&medium, WAIT_MS);
    }
    return chr_shelves_pack(shelves);
}

void chr_pack_carried_close(void)
{
    chr_shelves_close(shelves);
    shelves = NULL;
}

void chr_pack_carried_report(char *line, size_t capacity)
{
    chr_shelves_info_t info = {0};
    if (!shelves || !capacity) return;
    chr_shelves_info(shelves, &info);
    (void)snprintf(line, capacity, " loads=%u load_failed=%u load_max_ms=%u", (unsigned)info.loads, (unsigned)info.failed,
                   (unsigned)info.load_max_ms);
}
