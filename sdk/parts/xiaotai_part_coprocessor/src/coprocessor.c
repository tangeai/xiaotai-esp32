// SPDX-License-Identifier: MIT
/* Network part of a Wi-Fi coprocessor: creation, prepare() (link bring-up,
 * firmware check, automatic update) and coprocessor_info().
 *
 * The rules are the ones of xiaotai/parts/coprocessor.h. They are evaluated in
 * the order deployed firmware evaluates them, which differs from the
 * numbering in one place: the attempt marker (rule 7) only holds back a
 * transfer that rules 8 or 9 would start; firmware no rule may update is
 * UNSUPPORTED (rule 11) whether a marker is recorded or not.
 *
 * Two board shapes come from the configuration alone:
 *   - read-only check (no image): handshake, chip, protocol version. A
 *     version request that fails, or a version not accepted, is UNSUPPORTED;
 *     the application descriptor is not requested.
 *   - updater (image): image check first, then chip, version and descriptor;
 *     a request that fails is NO_LINK (retry), the pre-versioned protocol is
 *     recognised when the board allows it, and the marker guards transfers.
 * Every failure after a transfer started in this boot ends in POWER_CYCLE,
 * except a link that does not come up (NO_LINK). */
#include "coprocessor_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "coprocessor_link.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port/sys.h"

static const char *TAG = "coprocessor";

#define DEFAULT_LINK_TIMEOUT_MS 15000U
#define DEFAULT_ACTIVATE_WAIT_MS 50U
#define DEFAULT_LEGACY_ACTIVATE_WAIT_MS 6000U
#define DEFAULT_WRITE_BLOCK_BYTES 1024U
/* Storage wait of the attempt marker, as deployed firmware waits for it. */
#define MARKER_TIMEOUT_MS 3000U
/* Progress of the transfer: writing runs 5..90 %, then activation and the
 * hand-over to the restart, as the deployed progress screen shows them. */
#define PERCENT_VERIFY_IMAGE 1U
#define PERCENT_WRITE_FIRST 5U
#define PERCENT_WRITE_SPAN 85U
#define PERCENT_ACTIVATE 94U
#define PERCENT_RESTART 96U
/* Three 32-bit numbers, two dots and the terminator. */
#define PROTOCOL_TEXT 36U

/* ------------------------------------------------------------- image format */

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

xiaotai_err_t xiaotai_coprocessor_image_read(const uint8_t *data, size_t size,
                                             xiaotai_coprocessor_image_info_t *info)
{
    if (!data || !info || size < XIAOTAI_COPROCESSOR_IMAGE_MIN_BYTES) return XIAOTAI_ERR_INVALID_SIZE;
    memset(info, 0, sizeof(*info));
    info->magic = data[0];
    info->segment_count = data[1];
    info->chip_id = le16(data + 12);
    info->min_revision = le16(data + 15);
    info->max_revision = le16(data + 17);
    info->first_segment_bytes = le32(data + XIAOTAI_COPROCESSOR_IMAGE_HEADER_BYTES + 4U);
    const uint8_t *app = data + XIAOTAI_COPROCESSOR_IMAGE_HEADER_BYTES + XIAOTAI_COPROCESSOR_SEGMENT_HEADER_BYTES;
    info->app.magic = le32(app);
    memcpy(info->app.version, app + 16, XIAOTAI_COPROCESSOR_APP_TEXT);
    memcpy(info->app.project, app + 48, XIAOTAI_COPROCESSOR_APP_TEXT);
    memcpy(info->app.platform, app + 112, XIAOTAI_COPROCESSOR_APP_TEXT);
    memcpy(info->app.elf_sha256, app + 144, XIAOTAI_COPROCESSOR_DIGEST_BYTES);
    return XIAOTAI_OK;
}

bool xiaotai_coprocessor_text_terminated(const char text[XIAOTAI_COPROCESSOR_APP_TEXT])
{
    return memchr(text, 0, XIAOTAI_COPROCESSOR_APP_TEXT) != NULL;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool xiaotai_coprocessor_hex_digest(const char *hex, uint8_t out[XIAOTAI_COPROCESSOR_DIGEST_BYTES])
{
    if (!hex || strlen(hex) != 2U * XIAOTAI_COPROCESSOR_DIGEST_BYTES) return false;
    for (size_t i = 0; i < XIAOTAI_COPROCESSOR_DIGEST_BYTES; ++i) {
        const int high = hex_value(hex[2 * i]), low = hex_value(hex[2 * i + 1]);
        if (high < 0 || low < 0) return false;
        out[i] = (uint8_t)(high << 4 | low);
    }
    return true;
}

/* -------------------------------------------------------------- one run */

typedef struct {
    xiaotai_coprocessor_part_t *part;
    xiaotai_network_progress_fn_t progress;
    xiaotai_network_identity_fn_t identity;
    void *ctx;
} run_t;

static void report(run_t *run, xiaotai_network_stage_t stage, uint8_t percent, xiaotai_err_t err)
{
    xiaotai_coprocessor_part_t *part = run->part;
    xiaotai_critical_enter(&part->lock);
    part->state.stage = stage;
    part->state.error = err;
    xiaotai_critical_exit(&part->lock);
    if (run->progress) run->progress(stage, percent, run->ctx);
}

static void set_rule(xiaotai_coprocessor_part_t *part, uint8_t rule)
{
    xiaotai_critical_enter(&part->lock);
    part->state.rule = rule;
    xiaotai_critical_exit(&part->lock);
}

/* A final outcome exactly as given. */
static xiaotai_err_t conclude(run_t *run, xiaotai_network_stage_t stage, uint8_t rule, xiaotai_err_t err)
{
    set_rule(run->part, rule);
    report(run, stage, stage == XIAOTAI_NETWORK_READY ? 100U : 0U, err);
    return err;
}

/* A failure: after a transfer started in this boot the coprocessor may hold
 * part of an image, so every failure asks for a power cycle. */
static xiaotai_err_t fail(run_t *run, xiaotai_network_stage_t stage, uint8_t rule, xiaotai_err_t err)
{
    if (run->part->transfer_started) stage = XIAOTAI_NETWORK_POWER_CYCLE;
    XIAOTAI_LOGE(TAG, "check stopped: rule %u stage %d error 0x%x", (unsigned)rule, (int)stage, (unsigned)err);
    return conclude(run, stage, rule, err);
}

static void cache_clear(xiaotai_coprocessor_part_t *part)
{
    xiaotai_critical_enter(&part->lock);
    part->cached = false;
    memset(&part->cache, 0, sizeof(part->cache));
    xiaotai_critical_exit(&part->lock);
}

static void copy_text(char out[XIAOTAI_COPROCESSOR_TEXT_MAX], const char text[XIAOTAI_COPROCESSOR_APP_TEXT])
{
    size_t length = 0;
    while (length < XIAOTAI_COPROCESSOR_APP_TEXT && length + 1U < XIAOTAI_COPROCESSOR_TEXT_MAX && text[length]) {
        ++length;
    }
    memcpy(out, text, length);
    out[length] = '\0';
}

static void fill_info(xiaotai_coprocessor_info_t *info, const xiaotai_coprocessor_app_t *app, const char *protocol)
{
    memset(info, 0, sizeof(*info));
    copy_text(info->project, app->project);
    copy_text(info->version, app->version);
    copy_text(info->platform, app->platform);
    const size_t length = strnlen(protocol, sizeof(info->protocol) - 1U);
    memcpy(info->protocol, protocol, length);
    info->protocol[length] = '\0';
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < XIAOTAI_COPROCESSOR_DIGEST_BYTES; ++i) {
        info->elf_sha256[2 * i] = digits[app->elf_sha256[i] >> 4];
        info->elf_sha256[2 * i + 1] = digits[app->elf_sha256[i] & 0x0f];
    }
    info->elf_sha256[2 * XIAOTAI_COPROCESSOR_DIGEST_BYTES] = '\0';
}

static bool digest_zero(const uint8_t digest[XIAOTAI_COPROCESSOR_DIGEST_BYTES])
{
    for (size_t i = 0; i < XIAOTAI_COPROCESSOR_DIGEST_BYTES; ++i) {
        if (digest[i]) return false;
    }
    return true;
}

/* Keeps the running firmware's identity for coprocessor_info(live = false),
 * only when it is complete: a descriptor with its magic, three non-empty
 * terminated texts and a digest. Anything less leaves the cache empty. */
static void cache_set(run_t *run, const xiaotai_coprocessor_app_t *app, const char *protocol)
{
    xiaotai_coprocessor_part_t *part = run->part;
    const bool complete = app->magic == XIAOTAI_COPROCESSOR_APP_MAGIC &&
                          xiaotai_coprocessor_text_terminated(app->project) && app->project[0] &&
                          xiaotai_coprocessor_text_terminated(app->version) && app->version[0] &&
                          xiaotai_coprocessor_text_terminated(app->platform) && app->platform[0] &&
                          !digest_zero(app->elf_sha256);
    xiaotai_coprocessor_info_t info;
    if (complete) fill_info(&info, app, protocol);
    xiaotai_critical_enter(&part->lock);
    part->cached = complete;
    if (complete) part->cache = info;
    xiaotai_critical_exit(&part->lock);
    if (run->identity) run->identity(complete ? &info : NULL, run->ctx);
}

static void protocol_text(const xiaotai_coprocessor_version_t *version, char out[PROTOCOL_TEXT])
{
    (void)snprintf(out, PROTOCOL_TEXT, "%" PRIu32 ".%" PRIu32 ".%" PRIu32, version->major, version->minor,
                   version->patch);
}

/* NULL list: every protocol is accepted. */
static bool protocol_accepted(const xiaotai_coprocessor_config_t *config, const char *protocol)
{
    if (!config->protocols) return true;
    for (const char *const *p = config->protocols; *p; ++p) {
        if (strcmp(*p, protocol) == 0) return true;
    }
    return false;
}

static xiaotai_err_t link_up(xiaotai_coprocessor_part_t *part, bool manual_retry)
{
    const xiaotai_coprocessor_config_t *config = part->config;
    const uint32_t timeout = config->link_timeout_ms ? config->link_timeout_ms : DEFAULT_LINK_TIMEOUT_MS;
    if (!config->manual_link) return xiaotai_coprocessor_link_wait(timeout);
    if (manual_retry) {
        /* A retry re-enters the link's own lifecycle so its failed bring-up
         * state is not bypassed. */
        const xiaotai_err_t err = xiaotai_coprocessor_link_stop();
        if (err != XIAOTAI_OK) return err;
    }
    return xiaotai_coprocessor_link_start(timeout);
}

/* ----------------------------------------------------------- read-only check */

static xiaotai_err_t check_read_only(run_t *run)
{
    const xiaotai_coprocessor_config_t *config = run->part->config;
    const uint16_t chip = xiaotai_coprocessor_link_chip();
    if (chip != config->chip_id) {
        XIAOTAI_LOGE(TAG, "wrong coprocessor chip 0x%02x (expected 0x%02x)", (unsigned)chip, (unsigned)config->chip_id);
        return conclude(run, XIAOTAI_NETWORK_UNSUPPORTED, 3, XIAOTAI_ERR_INVALID_RESPONSE);
    }
    report(run, XIAOTAI_NETWORK_VERIFYING, 0, XIAOTAI_OK);
    xiaotai_coprocessor_version_t version = {0};
    xiaotai_err_t err = xiaotai_coprocessor_link_version(&version);
    if (err != XIAOTAI_OK) return conclude(run, XIAOTAI_NETWORK_UNSUPPORTED, 3, err);
    char protocol[PROTOCOL_TEXT];
    protocol_text(&version, protocol);
    if (!protocol_accepted(config, protocol)) {
        XIAOTAI_LOGE(TAG, "protocol %s not accepted; no automatic firmware update", protocol);
        return conclude(run, XIAOTAI_NETWORK_UNSUPPORTED, 3, XIAOTAI_ERR_INVALID_VERSION);
    }
    XIAOTAI_LOGI(TAG, "protocol %s; read-only check passed", protocol);
    return conclude(run, XIAOTAI_NETWORK_READY, 5, XIAOTAI_OK);
}

/* --------------------------------------------------------------- updater */

/* Rule 2: the board's image, before anything is asked of the coprocessor. */
static xiaotai_err_t check_image(const xiaotai_coprocessor_config_t *config, xiaotai_coprocessor_image_info_t *info)
{
    const xiaotai_coprocessor_image_t *image = config->image;
    if (xiaotai_coprocessor_image_read(image->data, image->size, info) != XIAOTAI_OK) return XIAOTAI_ERR_INVALID_SIZE;
    uint8_t expected[XIAOTAI_COPROCESSOR_DIGEST_BYTES], digest[XIAOTAI_SHA256_BYTES];
    if (!xiaotai_coprocessor_hex_digest(image->sha256, expected) ||
        xiaotai_sha256(image->data, image->size, digest) != XIAOTAI_OK ||
        memcmp(digest, expected, sizeof(expected)) != 0) {
        return XIAOTAI_ERR_INVALID_CRC;
    }
    if (info->magic != XIAOTAI_COPROCESSOR_IMAGE_MAGIC || info->chip_id != config->chip_id ||
        info->segment_count == 0) {
        return XIAOTAI_ERR_INVALID_RESPONSE;
    }
    if (config->chip_revision &&
        (info->min_revision > config->chip_revision || info->max_revision < config->chip_revision)) {
        return XIAOTAI_ERR_INVALID_RESPONSE;
    }
    if (info->app.magic != XIAOTAI_COPROCESSOR_APP_MAGIC || !xiaotai_coprocessor_text_terminated(info->app.project) ||
        !xiaotai_coprocessor_text_terminated(info->app.version) || strcmp(info->app.project, image->project) != 0 ||
        strcmp(info->app.version, image->version) != 0) {
        return XIAOTAI_ERR_INVALID_RESPONSE;
    }
    return XIAOTAI_OK;
}

/* Rule 9's protocol family: the handshake carries the board's capability word
 * and nothing newer, and the request only that protocol answers in a known
 * way before Wi-Fi starts gets that answer. Silence is no evidence. */
static bool unversioned_protocol(const xiaotai_coprocessor_config_t *config)
{
    if (!config->legacy_protocol_capability) return false;
    xiaotai_coprocessor_link_facts_t facts = {0};
    xiaotai_coprocessor_link_facts(&facts);
    if (facts.firmware != 0 || facts.capabilities != config->legacy_protocol_capability ||
        facts.ext_capabilities != 0 || facts.rpc_version_advertised || facts.bus_buffer_config) {
        return false;
    }
    bool matched = false;
    return xiaotai_coprocessor_link_probe_unversioned(&matched) == XIAOTAI_OK && matched;
}

static bool target_running(const xiaotai_coprocessor_app_t *running, const xiaotai_coprocessor_app_t *target)
{
    return xiaotai_coprocessor_text_terminated(running->project) &&
           xiaotai_coprocessor_text_terminated(running->version) &&
           running->magic == XIAOTAI_COPROCESSOR_APP_MAGIC && strcmp(running->project, target->project) == 0 &&
           strcmp(running->version, target->version) == 0 &&
           memcmp(running->elf_sha256, target->elf_sha256, XIAOTAI_COPROCESSOR_DIGEST_BYTES) == 0;
}

/* Rule 6: firmware older than descriptors with a magic, known to work. */
static bool legacy_firmware(const xiaotai_coprocessor_config_t *config, const xiaotai_coprocessor_app_t *running)
{
    return config->legacy.project && running->magic == 0 && strcmp(running->project, config->legacy.project) == 0 &&
           (!config->legacy.version || strcmp(running->version, config->legacy.version) == 0);
}

/* Rule 8: an entry of sources whose verified limit holds the image. */
static const xiaotai_coprocessor_source_t *find_source(const xiaotai_coprocessor_config_t *config,
                                                       const xiaotai_coprocessor_app_t *running, size_t image_size)
{
    if (running->magic != XIAOTAI_COPROCESSOR_APP_MAGIC) return NULL;
    for (size_t i = 0; config->sources && i < config->source_count; ++i) {
        const xiaotai_coprocessor_source_t *source = &config->sources[i];
        uint8_t elf[XIAOTAI_COPROCESSOR_DIGEST_BYTES];
        if (source->image_max == 0 || source->image_max < image_size || !source->project || !source->version ||
            strcmp(running->project, source->project) != 0 || strcmp(running->version, source->version) != 0) {
            continue;
        }
        if (source->elf_sha256 && (!xiaotai_coprocessor_hex_digest(source->elf_sha256, elf) ||
                                   memcmp(elf, running->elf_sha256, sizeof(elf)) != 0)) {
            continue;
        }
        return source;
    }
    return NULL;
}

static xiaotai_err_t marker_record(const xiaotai_coprocessor_config_t *config)
{
    xiaotai_err_t err = xiaotai_kv_set_u8(config->attempt_space, config->attempt_key, 1, MARKER_TIMEOUT_MS);
    if (err == XIAOTAI_OK) err = xiaotai_kv_flush(MARKER_TIMEOUT_MS);
    return err;
}

/* Sends the image. A block that fails is never resent and the transfer is not
 * ended: the coprocessor may have appended it, and end is no cancellation.
 * *activation: from here on the coprocessor may be switching firmware, so the
 * host must restart whatever the result. */
static xiaotai_err_t transfer(run_t *run, bool legacy, bool *activation)
{
    const xiaotai_coprocessor_config_t *config = run->part->config;
    const uint8_t *data = config->image->data;
    const size_t size = config->image->size;
    const size_t block = config->write_block_bytes ? config->write_block_bytes : DEFAULT_WRITE_BLOCK_BYTES;
    *activation = false;
    xiaotai_err_t err = xiaotai_coprocessor_link_ota_begin();
    if (err != XIAOTAI_OK) return err;
    report(run, XIAOTAI_NETWORK_WRITING, PERCENT_WRITE_FIRST, XIAOTAI_OK);
    for (size_t offset = 0; offset < size; offset += block) {
        const size_t count = size - offset < block ? size - offset : block;
        err = xiaotai_coprocessor_link_ota_write(data + offset, count);
        if (err != XIAOTAI_OK) {
            XIAOTAI_LOGE(TAG, "write result uncertain at offset %u", (unsigned)offset);
            return err;
        }
        report(run, XIAOTAI_NETWORK_WRITING,
               (uint8_t)(PERCENT_WRITE_FIRST + (offset + count) * PERCENT_WRITE_SPAN / size), XIAOTAI_OK);
        xiaotai_delay_ms(1);
    }
    /* The pre-versioned protocol commits and restarts inside end, possibly
     * before it answers. */
    *activation = legacy;
    err = xiaotai_coprocessor_link_ota_end();
    if (err != XIAOTAI_OK || legacy) return err;
    report(run, XIAOTAI_NETWORK_ACTIVATING, PERCENT_ACTIVATE, XIAOTAI_OK);
    *activation = true;
    return xiaotai_coprocessor_link_ota_activate();
}

static xiaotai_err_t check_and_update(run_t *run, bool manual_retry)
{
    xiaotai_coprocessor_part_t *part = run->part;
    const xiaotai_coprocessor_config_t *config = part->config;
    report(run, XIAOTAI_NETWORK_VERIFYING, PERCENT_VERIFY_IMAGE, XIAOTAI_OK);
    xiaotai_coprocessor_image_info_t target;
    xiaotai_err_t err = check_image(config, &target);
    if (err != XIAOTAI_OK) return fail(run, XIAOTAI_NETWORK_BAD_IMAGE, 2, err);
    const size_t image_size = config->image->size;

    const uint16_t chip = xiaotai_coprocessor_link_chip();
    if (chip != config->chip_id) {
        /* No chip id yet means the handshake is incomplete: retry. */
        return fail(run, chip ? XIAOTAI_NETWORK_UNSUPPORTED : XIAOTAI_NETWORK_NO_LINK, chip ? 3 : 1,
                    XIAOTAI_ERR_INVALID_RESPONSE);
    }
    xiaotai_coprocessor_version_t version = {0};
    xiaotai_coprocessor_app_t running = {0};
    bool legacy = false;
    err = xiaotai_coprocessor_link_version(&version);
    if (err != XIAOTAI_OK) {
        if (!unversioned_protocol(config)) return fail(run, XIAOTAI_NETWORK_NO_LINK, 1, err);
        /* That protocol also predates the descriptor request: firmware that
         * answers it is something else, partly broken. */
        xiaotai_coprocessor_app_t probe = {0};
        if (xiaotai_coprocessor_link_app(&probe) == XIAOTAI_OK) {
            return fail(run, XIAOTAI_NETWORK_NO_LINK, 1, XIAOTAI_ERR_INVALID_RESPONSE);
        }
        legacy = true;
        XIAOTAI_LOGI(TAG, "pre-versioned protocol recognised");
    } else {
        /* The descriptor is a request of its own; never inferred from the version. */
        err = xiaotai_coprocessor_link_app(&running);
        if (err != XIAOTAI_OK) return fail(run, XIAOTAI_NETWORK_NO_LINK, 1, err);
        if (!xiaotai_coprocessor_text_terminated(running.project) ||
            !xiaotai_coprocessor_text_terminated(running.version)) {
            return fail(run, XIAOTAI_NETWORK_NO_LINK, 1, XIAOTAI_ERR_INVALID_RESPONSE);
        }
    }
    char protocol[PROTOCOL_TEXT] = "";
    if (!legacy) {
        protocol_text(&version, protocol);
        cache_set(run, &running, protocol);
    }
    xiaotai_critical_enter(&part->lock);
    part->state.legacy_protocol = legacy;
    xiaotai_critical_exit(&part->lock);

    uint8_t attempt = 0;
    err = xiaotai_kv_get_u8(config->attempt_space, config->attempt_key, &attempt, MARKER_TIMEOUT_MS);
    if (err == XIAOTAI_ERR_NOT_FOUND) {
        attempt = 0;
        err = XIAOTAI_OK;
    }
    if (err != XIAOTAI_OK) return fail(run, XIAOTAI_NETWORK_NO_LINK, 7, err);
    xiaotai_critical_enter(&part->lock);
    part->state.attempt_recorded = attempt != 0;
    xiaotai_critical_exit(&part->lock);

    const bool accepted = !legacy && protocol_accepted(config, protocol);
    if (!legacy && accepted && target_running(&running, &target.app)) {
        if (attempt) {
            err = xiaotai_kv_erase(config->attempt_space, config->attempt_key, MARKER_TIMEOUT_MS);
            if (err != XIAOTAI_OK) return fail(run, XIAOTAI_NETWORK_NO_LINK, 4, err);
            xiaotai_critical_enter(&part->lock);
            part->state.attempt_recorded = false;
            xiaotai_critical_exit(&part->lock);
        }
        XIAOTAI_LOGI(TAG, "firmware %s verified%s; no update", target.app.version, attempt ? " after activation" : "");
        return conclude(run, XIAOTAI_NETWORK_READY, 4, XIAOTAI_OK);
    }
    if (!legacy && accepted && !attempt && !part->transfer_started && legacy_firmware(config, &running)) {
        XIAOTAI_LOGW(TAG, "older firmware without a complete identity; left running");
        return conclude(run, XIAOTAI_NETWORK_READY, 6, XIAOTAI_OK);
    }
    if (part->transfer_started) {
        XIAOTAI_LOGW(TAG, "a transfer already started in this boot; power cycle required");
        return fail(run, XIAOTAI_NETWORK_POWER_CYCLE, part->transfer_rule, XIAOTAI_ERR_INVALID_STATE);
    }
    const xiaotai_coprocessor_source_t *source = !legacy && accepted ? find_source(config, &running, image_size) : NULL;
    if (!legacy && !source && !attempt && accepted &&
        (running.magic == XIAOTAI_COPROCESSOR_APP_MAGIC || running.magic == 0) && running.project[0] &&
        running.version[0]) {
        /* Unknown firmware on an accepted protocol is no update source; the
         * network may still work with it. */
        XIAOTAI_LOGW(TAG, "firmware %s %s is no update source; left running", running.project, running.version);
        return conclude(run, XIAOTAI_NETWORK_READY, 10, XIAOTAI_OK);
    }
    if ((legacy && image_size > config->legacy_image_max) || (!legacy && !source)) {
        XIAOTAI_LOGE(TAG, "no update path: legacy %u protocol %s image %u", (unsigned)legacy, protocol,
                     (unsigned)image_size);
        return fail(run, XIAOTAI_NETWORK_UNSUPPORTED, legacy || accepted ? 11 : 3, XIAOTAI_ERR_NOT_SUPPORTED);
    }
    if (attempt && !manual_retry) {
        XIAOTAI_LOGW(TAG, "an earlier update is unconfirmed; waiting for a manual retry");
        return fail(run, XIAOTAI_NETWORK_UNFINISHED, 7, XIAOTAI_ERR_INVALID_STATE);
    }
    const uint8_t rule = legacy ? 9 : 8;
    /* The marker is durable before the first block: if power fails during
     * the transfer, the next boot waits for a manual retry. */
    err = marker_record(config);
    if (err != XIAOTAI_OK) return fail(run, XIAOTAI_NETWORK_NO_LINK, rule, err);
    part->transfer_started = true;
    part->transfer_rule = rule;
    xiaotai_critical_enter(&part->lock);
    part->state.attempt_recorded = true;
    part->state.transfer_started = true;
    part->state.rule = rule;
    xiaotai_critical_exit(&part->lock);
    /* Only the next boot's check may confirm what runs after a transfer. */
    cache_clear(part);
    if (run->identity) run->identity(NULL, run->ctx);
    XIAOTAI_LOGI(TAG, "update from %s, %u bytes", legacy ? "the pre-versioned protocol" : running.version,
                 (unsigned)image_size);

    bool activation = false;
    err = transfer(run, legacy, &activation);
    if (err != XIAOTAI_OK && !activation) return fail(run, XIAOTAI_NETWORK_POWER_CYCLE, rule, err);
    /* Activation restarts the coprocessor's end of the link: the host restarts
     * too and the next boot verifies the firmware before clearing the marker.
     * The pre-versioned firmware restarts itself seconds after end, so the
     * host must not reset the link while it still runs the old image. */
    set_rule(part, rule);
    report(run, XIAOTAI_NETWORK_RESTARTING, PERCENT_RESTART, err);
    const uint32_t wait = legacy ? (config->legacy_activate_wait_ms ? config->legacy_activate_wait_ms
                                                                    : DEFAULT_LEGACY_ACTIVATE_WAIT_MS)
                                 : (config->activate_wait_ms ? config->activate_wait_ms : DEFAULT_ACTIVATE_WAIT_MS);
    XIAOTAI_LOGI(TAG, "activation submitted (0x%x); host restart after %u ms", (unsigned)err, (unsigned)wait);
    xiaotai_delay_ms(wait);
    return XIAOTAI_OK;
}

/* ------------------------------------------------------------------- hooks */

static xiaotai_err_t prepare_with_identity(void *self, bool manual_retry, xiaotai_network_progress_fn_t progress,
                                          xiaotai_network_identity_fn_t identity, void *ctx)
{
    xiaotai_coprocessor_part_t *part = self;
    run_t run = {.part = part, .progress = progress, .identity = identity, .ctx = ctx};
    cache_clear(part);
    xiaotai_critical_enter(&part->lock);
    part->state.legacy_protocol = false;
    xiaotai_critical_exit(&part->lock);
    report(&run, XIAOTAI_NETWORK_CHECKING, 0, XIAOTAI_OK);
    const xiaotai_err_t err = link_up(part, manual_retry);
    if (err != XIAOTAI_OK) {
        XIAOTAI_LOGE(TAG, "link not ready (0x%x)", (unsigned)err);
        return conclude(&run, XIAOTAI_NETWORK_NO_LINK, 1, err);
    }
    return part->config->image ? check_and_update(&run, manual_retry) : check_read_only(&run);
}

static xiaotai_err_t prepare(void *self, bool manual_retry, xiaotai_network_progress_fn_t progress, void *ctx)
{
    return prepare_with_identity(self, manual_retry, progress, NULL, ctx);
}

static xiaotai_err_t coprocessor_info(void *self, bool live, xiaotai_coprocessor_info_t *info)
{
    xiaotai_coprocessor_part_t *part = self;
    if (!info) return XIAOTAI_ERR_INVALID_ARG;
    if (!live) {
        xiaotai_err_t err = XIAOTAI_ERR_NOT_FOUND;
        xiaotai_critical_enter(&part->lock);
        if (part->cached) {
            *info = part->cache;
            err = XIAOTAI_OK;
        }
        xiaotai_critical_exit(&part->lock);
        return err;
    }
    xiaotai_coprocessor_version_t version = {0};
    xiaotai_err_t err = xiaotai_coprocessor_link_version(&version);
    if (err != XIAOTAI_OK) return err;
    xiaotai_coprocessor_app_t app = {0};
    err = xiaotai_coprocessor_link_app(&app);
    if (err != XIAOTAI_OK) return err;
    char protocol[PROTOCOL_TEXT];
    protocol_text(&version, protocol);
    fill_info(info, &app, protocol);
    return XIAOTAI_OK;
}

/* ------------------------------------------------------------------ create */

static bool in_list(const char *const *list, const char *word)
{
    for (; list && *list; ++list) {
        if (strcmp(*list, word) == 0) return true;
    }
    return false;
}

static bool kv_name(const char *name) { return name && name[0] && strlen(name) <= XIAOTAI_KV_NAME_MAX; }

static bool config_valid(const xiaotai_coprocessor_config_t *config)
{
    for (const char *const *word = config->restart_commands; word && *word; ++word) {
        if (!in_list(config->commands, *word)) return false;
    }
    if (!config->image) return true;
    uint8_t digest[XIAOTAI_COPROCESSOR_DIGEST_BYTES];
    const xiaotai_coprocessor_image_t *image = config->image;
    if (!image->data || !image->project || !image->version || !xiaotai_coprocessor_hex_digest(image->sha256, digest) ||
        !kv_name(config->attempt_space) || !kv_name(config->attempt_key) ||
        (config->source_count && !config->sources)) {
        return false;
    }
    for (size_t i = 0; i < config->source_count; ++i) {
        const xiaotai_coprocessor_source_t *source = &config->sources[i];
        if (!source->project || !source->version ||
            (source->elf_sha256 && !xiaotai_coprocessor_hex_digest(source->elf_sha256, digest))) {
            return false;
        }
    }
    return true;
}

xiaotai_err_t xiaotai_coprocessor_create(const xiaotai_coprocessor_config_t *config,
                                         const xiaotai_network_t **network)
{
    if (!network) return XIAOTAI_ERR_INVALID_ARG;
    *network = NULL;
    if (!config || !config_valid(config)) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_coprocessor_part_t *part = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*part));
    if (!part) return XIAOTAI_ERR_NO_MEM;
    part->config = config;
    xiaotai_critical_init(&part->lock);
    part->state.stage = XIAOTAI_NETWORK_CHECKING;
    part->network.desc.kind = XIAOTAI_NETWORK_WIFI_COPROCESSOR;
    part->network.desc.provisioning = config->provisioning;
    part->network.driver_config = config->wifi;
    part->network.prepare = prepare;
    part->network.coprocessor_info = coprocessor_info;
    if (config->image) part->network.prepare_with_identity = prepare_with_identity;
    if (config->update_projects) part->network.coprocessor_update = xiaotai_coprocessor_manual_update;
    if (config->command_channel && config->commands) part->network.coprocessor_command = xiaotai_coprocessor_command;
    part->network.self = part;
    *network = &part->network;
    return XIAOTAI_OK;
}

void xiaotai_coprocessor_state(const xiaotai_network_t *network, xiaotai_coprocessor_state_t *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    if (!network || network->prepare != prepare || !network->self) return;
    xiaotai_coprocessor_part_t *part = network->self;
    xiaotai_critical_enter(&part->lock);
    *state = part->state;
    xiaotai_critical_exit(&part->lock);
}
