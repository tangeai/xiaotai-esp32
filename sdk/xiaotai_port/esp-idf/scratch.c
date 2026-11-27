// SPDX-License-Identifier: MIT
/* xiaotai/port/scratch.h on ESP-IDF: the data partition named
 * XIAOTAI_SCRATCH_PARTITION, where a board's partition table has one. The
 * flash driver refuses to write into the partition the application runs from,
 * so the scratch is a partition of its own. */
#include "xiaotai/port/scratch.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_partition.h"
#include "xiaotai/port/os.h"

static const esp_partition_t *s_part;
static bool s_known;

static void find(void)
{
    if (s_known) return;
    s_known = true;
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, XIAOTAI_SCRATCH_PARTITION);
    if (s_part && (s_part->readonly || s_part->size < XIAOTAI_SCRATCH_SECTOR)) s_part = NULL;
}

size_t xiaotai_scratch_size(void)
{
    find();
    return s_part ? s_part->size : 0;
}

static xiaotai_err_t check(size_t offset, size_t bytes)
{
    if (!s_part) return XIAOTAI_ERR_NOT_SUPPORTED;
    return offset <= s_part->size && bytes <= s_part->size - offset ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_ARG;
}

xiaotai_err_t xiaotai_scratch_erase(size_t offset, size_t bytes)
{
    find();
    xiaotai_err_t error = check(offset, bytes);
    if (error == XIAOTAI_OK && (offset % XIAOTAI_SCRATCH_SECTOR || bytes % XIAOTAI_SCRATCH_SECTOR)) error = XIAOTAI_ERR_INVALID_ARG;
    if (error == XIAOTAI_OK) error = xiaotai_require_internal_stack("scratch.erase");
    if (error != XIAOTAI_OK) return error;
    return esp_partition_erase_range(s_part, offset, bytes) == ESP_OK ? XIAOTAI_OK : XIAOTAI_FAIL;
}

xiaotai_err_t xiaotai_scratch_write(size_t offset, const void *data, size_t bytes)
{
    find();
    xiaotai_err_t error = data ? check(offset, bytes) : XIAOTAI_ERR_INVALID_ARG;
    if (error == XIAOTAI_OK) error = xiaotai_require_internal_stack("scratch.write");
    if (error != XIAOTAI_OK) return error;
    return esp_partition_write(s_part, offset, data, bytes) == ESP_OK ? XIAOTAI_OK : XIAOTAI_FAIL;
}

xiaotai_err_t xiaotai_scratch_read(size_t offset, void *data, size_t bytes)
{
    find();
    xiaotai_err_t error = data ? check(offset, bytes) : XIAOTAI_ERR_INVALID_ARG;
    if (error == XIAOTAI_OK) error = xiaotai_require_internal_stack("scratch.read");
    if (error != XIAOTAI_OK) return error;
    return esp_partition_read(s_part, offset, data, bytes) == ESP_OK ? XIAOTAI_OK : XIAOTAI_FAIL;
}
