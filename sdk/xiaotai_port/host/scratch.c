// SPDX-License-Identifier: MIT
/* xiaotai/port/scratch.h on the host: memory that behaves like flash (erased
 * to 0xff, writing only clears bits), none until a test asks for some with
 * xiaotai_scratch_host_set_size(). */
#include "xiaotai/port/scratch.h"
#include "xiaotai_scratch_host.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *s_flash;
static size_t s_size;
static xiaotai_scratch_host_stats_t s_stats;

void xiaotai_scratch_host_set_size(size_t bytes)
{
    pthread_mutex_lock(&s_lock);
    free(s_flash);
    s_flash = bytes ? malloc(bytes) : NULL;
    s_size = s_flash ? bytes : 0;
    if (s_flash) memset(s_flash, 0x55, s_size);      /* not erased until somebody erases it */
    s_stats = (xiaotai_scratch_host_stats_t){0};
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_scratch_host_stats(xiaotai_scratch_host_stats_t *stats)
{
    pthread_mutex_lock(&s_lock);
    *stats = s_stats;
    pthread_mutex_unlock(&s_lock);
}

size_t xiaotai_scratch_size(void)
{
    pthread_mutex_lock(&s_lock);
    const size_t size = s_size;
    pthread_mutex_unlock(&s_lock);
    return size;
}

static xiaotai_err_t check(size_t offset, size_t bytes)
{
    if (!s_size) return XIAOTAI_ERR_NOT_SUPPORTED;
    return offset <= s_size && bytes <= s_size - offset ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_ARG;
}

xiaotai_err_t xiaotai_scratch_erase(size_t offset, size_t bytes)
{
    pthread_mutex_lock(&s_lock);
    xiaotai_err_t error = check(offset, bytes);
    if (error == XIAOTAI_OK && (offset % XIAOTAI_SCRATCH_SECTOR || bytes % XIAOTAI_SCRATCH_SECTOR)) error = XIAOTAI_ERR_INVALID_ARG;
    if (error == XIAOTAI_OK) {
        memset(s_flash + offset, 0xff, bytes);
        ++s_stats.erases;
    }
    pthread_mutex_unlock(&s_lock);
    return error;
}

xiaotai_err_t xiaotai_scratch_write(size_t offset, const void *data, size_t bytes)
{
    pthread_mutex_lock(&s_lock);
    xiaotai_err_t error = data ? check(offset, bytes) : XIAOTAI_ERR_INVALID_ARG;
    for (size_t i = 0; error == XIAOTAI_OK && i < bytes; ++i) {
        const uint8_t value = ((const uint8_t *)data)[i];
        if ((s_flash[offset + i] & value) != value) {
            ++s_stats.unerased_writes;      /* flash can only clear bits */
            error = XIAOTAI_FAIL;
        } else s_flash[offset + i] = value;
    }
    if (error == XIAOTAI_OK) ++s_stats.writes;
    pthread_mutex_unlock(&s_lock);
    return error;
}

xiaotai_err_t xiaotai_scratch_read(size_t offset, void *data, size_t bytes)
{
    pthread_mutex_lock(&s_lock);
    xiaotai_err_t error = data ? check(offset, bytes) : XIAOTAI_ERR_INVALID_ARG;
    if (error == XIAOTAI_OK) {
        memcpy(data, s_flash + offset, bytes);
        ++s_stats.reads;
    }
    pthread_mutex_unlock(&s_lock);
    return error;
}
