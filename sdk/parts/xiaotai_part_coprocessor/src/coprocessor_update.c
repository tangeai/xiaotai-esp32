// SPDX-License-Identifier: MIT
#include "coprocessor_internal.h"
#include "xiaotai/port/log.h"
#include <string.h>

#define IMAGE_MAX_SEGMENTS 16U
#define DEFAULT_UPDATE_BLOCK_BYTES 512U
#define DEFAULT_UPDATE_RESTART_WAIT_MS 3000U

static bool in_list(const char *const *list, const char *word)
{
    for (; list && *list; ++list) if (strcmp(*list, word) == 0) return true;
    return false;
}

static xiaotai_err_t verify(const xiaotai_coprocessor_config_t *config, const uint8_t *data, size_t size)
{
    if (!data || size > config->update_image_max) return XIAOTAI_ERR_INVALID_SIZE;
    xiaotai_coprocessor_image_info_t info;
    const xiaotai_err_t err = xiaotai_coprocessor_image_read(data, size, &info);
    if (err != XIAOTAI_OK) return err;
    const size_t app_offset = XIAOTAI_COPROCESSOR_IMAGE_HEADER_BYTES + XIAOTAI_COPROCESSOR_SEGMENT_HEADER_BYTES;
    if (info.magic != XIAOTAI_COPROCESSOR_IMAGE_MAGIC || !info.segment_count ||
        info.segment_count > IMAGE_MAX_SEGMENTS || info.chip_id != config->chip_id ||
        info.first_segment_bytes < XIAOTAI_COPROCESSOR_APP_DESC_BYTES || info.first_segment_bytes > size - app_offset ||
        (config->chip_revision && (info.min_revision > config->chip_revision || info.max_revision < config->chip_revision)) ||
        info.app.magic != XIAOTAI_COPROCESSOR_APP_MAGIC || !info.app.project[0] ||
        !xiaotai_coprocessor_text_terminated(info.app.project) || !in_list(config->update_projects, info.app.project)) {
        return XIAOTAI_ERR_INVALID_VERSION;
    }
    return XIAOTAI_OK;
}

static void report(xiaotai_coprocessor_part_t *part, xiaotai_network_progress_fn_t progress, void *ctx,
                   xiaotai_network_stage_t stage, uint8_t percent, xiaotai_err_t error)
{
    xiaotai_critical_enter(&part->lock);
    part->state.stage = stage;
    part->state.error = error;
    xiaotai_critical_exit(&part->lock);
    if (progress) progress(stage, percent, ctx);
}

xiaotai_err_t xiaotai_coprocessor_manual_update(void *self, const uint8_t *data, size_t size,
                                               xiaotai_network_progress_fn_t progress, void *ctx)
{
    xiaotai_coprocessor_part_t *part = self;
    const xiaotai_coprocessor_config_t *config = part->config;
    if (part->transfer_started) {
        report(part, progress, ctx, XIAOTAI_NETWORK_POWER_CYCLE, 0, XIAOTAI_ERR_INVALID_STATE);
        return XIAOTAI_ERR_INVALID_STATE;
    }
    report(part, progress, ctx, XIAOTAI_NETWORK_VERIFYING, 0, XIAOTAI_OK);
    xiaotai_err_t err = verify(config, data, size);
    if (err != XIAOTAI_OK) {
        report(part, progress, ctx, XIAOTAI_NETWORK_BAD_IMAGE, 0, err);
        return err;
    }
    /* The download hash belongs to the caller. Confirm the attached peer too,
     * before issuing any instruction that could erase an inactive slot. */
    const uint16_t chip = xiaotai_coprocessor_link_chip();
    if (chip != config->chip_id) {
        const xiaotai_network_stage_t stage = chip ? XIAOTAI_NETWORK_UNSUPPORTED : XIAOTAI_NETWORK_NO_LINK;
        report(part, progress, ctx, stage, 0, XIAOTAI_ERR_INVALID_RESPONSE);
        return XIAOTAI_ERR_INVALID_RESPONSE;
    }
    const size_t block = config->update_block_bytes ? config->update_block_bytes : DEFAULT_UPDATE_BLOCK_BYTES;
    /* Once begin is submitted, even an error may mean the peer began to erase.
     * A later attempt must not continue the same boot on an uncertain slot. */
    part->transfer_started = true;
    xiaotai_critical_enter(&part->lock);
    part->state.transfer_started = true;
    part->cached = false;
    xiaotai_critical_exit(&part->lock);
    size_t written = 0;
    err = xiaotai_coprocessor_link_ota_begin();
    if (err != XIAOTAI_OK) goto failed;
    report(part, progress, ctx, XIAOTAI_NETWORK_WRITING, 5, XIAOTAI_OK);
    while (written < size) {
        const size_t n = size - written < block ? size - written : block;
        err = xiaotai_coprocessor_link_ota_write(data + written, n);
        if (err != XIAOTAI_OK) goto failed;
        written += n;
        report(part, progress, ctx, XIAOTAI_NETWORK_WRITING,
               (uint8_t)(5U + (uint64_t)written * 85U / size), XIAOTAI_OK);
    }
    err = xiaotai_coprocessor_link_ota_end();
    if (err != XIAOTAI_OK) goto failed;
    report(part, progress, ctx, XIAOTAI_NETWORK_ACTIVATING, 94, XIAOTAI_OK);
    err = xiaotai_coprocessor_link_ota_activate();
    if (err != XIAOTAI_OK) goto failed;
    report(part, progress, ctx, XIAOTAI_NETWORK_RESTARTING, 96, XIAOTAI_OK);
    xiaotai_delay_ms(config->update_restart_wait_ms ? config->update_restart_wait_ms : DEFAULT_UPDATE_RESTART_WAIT_MS);
    return XIAOTAI_OK;

failed:
    XIAOTAI_LOGE("coprocessor", "manual update stopped after %u bytes (error 0x%x)", (unsigned)written, (unsigned)err);
    report(part, progress, ctx, XIAOTAI_NETWORK_POWER_CYCLE, 0, err);
    return err;
}

xiaotai_err_t xiaotai_coprocessor_command(void *self, const char *command, char *reply, size_t capacity)
{
    const xiaotai_coprocessor_part_t *part = self;
    const xiaotai_coprocessor_config_t *config = part->config;
    if (capacity && !reply) return XIAOTAI_ERR_INVALID_ARG;
    if (capacity) reply[0] = '\0';
    if (!command || !command[0] || !in_list(config->commands, command)) return XIAOTAI_ERR_INVALID_ARG;
    const bool restart = in_list(config->restart_commands, command);
    const xiaotai_err_t err = xiaotai_coprocessor_link_command(config->command_channel, command, !restart,
                                                             reply, capacity);
    if (err == XIAOTAI_OK && restart && config->restart_command_wait_ms) {
        xiaotai_delay_ms(config->restart_command_wait_ms);
    }
    return err;
}
