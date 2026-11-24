// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_p4_companion_identity.py
// Replaces legacy: tests/regression/test_c6_ota_recovery.py
/* part_coprocessor: the decision rules of prepare() one by one, the attempt
 * marker and its order against the transfer, the transfer's failure
 * handling, the read-back on the next boot, link bring-up (timeout, manual
 * retry stops before it starts) and coprocessor_info().
 *
 * Ported from the deployed firmware's tests: the updater's recovery test
 * (decision rules, attempt latch, write and end timeouts, activation read-back)
 * and the bring-up part of the maintenance test (link start, retry). The two
 * configurations below carry the values of the two deployed board shapes: an
 * updater with a built-in image (synthetic here, same layout) and a
 * read-only check over a manually started link. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "coprocessor_internal.h"
#include "xiaotai/parts/coprocessor.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port/sys.h"
#include "xiaotai_coprocessor_host.h"
#include "xiaotai_mem_host.h"

#define CHECK(condition)                                                                        \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);       \
            exit(1);                                                                            \
        }                                                                                       \
    } while (0)

#define CHECK_TEXT(actual, expected)                                                             \
    do {                                                                                        \
        const char *a_ = (actual), *e_ = (expected);                                            \
        if (strcmp(a_, e_) != 0) {                                                              \
            fprintf(stderr, "%s:%d: text differs\n--- actual\n%s--- expected\n%s---\n", __FILE__, \
                    __LINE__, a_, e_);                                                          \
            exit(1);                                                                            \
        }                                                                                       \
    } while (0)

static xiaotai_coprocessor_host_t *H;

/* ------------------------------------------------- attempt marker store */

static bool s_marker_present;
static uint8_t s_marker;
static xiaotai_err_t s_get_result, s_set_result, s_flush_result, s_erase_result;
static unsigned s_kv_writes;

xiaotai_err_t xiaotai_kv_get_u8(const char *space, const char *key, uint8_t *value, uint32_t timeout_ms)
{
    xiaotai_coprocessor_host_note("kv get %s/%s %lu", space, key, (unsigned long)timeout_ms);
    if (s_get_result != XIAOTAI_OK) return s_get_result;
    if (!s_marker_present) return XIAOTAI_ERR_NOT_FOUND;
    *value = s_marker;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_kv_set_u8(const char *space, const char *key, uint8_t value, uint32_t timeout_ms)
{
    xiaotai_coprocessor_host_note("kv set %s/%s=%u %lu", space, key, (unsigned)value, (unsigned long)timeout_ms);
    ++s_kv_writes;
    if (s_set_result != XIAOTAI_OK) return s_set_result;
    s_marker_present = true;
    s_marker = value;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_kv_erase(const char *space, const char *key, uint32_t timeout_ms)
{
    xiaotai_coprocessor_host_note("kv erase %s/%s %lu", space, key, (unsigned long)timeout_ms);
    ++s_kv_writes;
    if (s_erase_result != XIAOTAI_OK) return s_erase_result;
    s_marker_present = false;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_kv_flush(uint32_t timeout_ms)
{
    xiaotai_coprocessor_host_note("kv flush %lu", (unsigned long)timeout_ms);
    return s_flush_result;
}

static void marker(bool present)
{
    s_marker_present = present;
    s_marker = present ? 1 : 0;
}

/* -------------------------------------------------------------- progress */

typedef struct {
    xiaotai_network_stage_t stage;
    uint8_t percent;
} step_t;

static step_t s_steps[64];
static size_t s_step_count;
static int s_progress_ctx;

static void progress(xiaotai_network_stage_t stage, uint8_t percent, void *ctx)
{
    CHECK(ctx == &s_progress_ctx);
    CHECK(percent <= 100);
    if (s_step_count < sizeof(s_steps) / sizeof(s_steps[0])) s_steps[s_step_count] = (step_t){stage, percent};
    ++s_step_count;
}

static void check_steps(const step_t *expected, size_t count)
{
    if (s_step_count != count) {
        fprintf(stderr, "progress: %zu steps, expected %zu:", s_step_count, count);
        for (size_t i = 0; i < s_step_count && i < 64; ++i) fprintf(stderr, " %d/%u", s_steps[i].stage, s_steps[i].percent);
        fputc('\n', stderr);
        exit(1);
    }
    for (size_t i = 0; i < count; ++i) {
        if (s_steps[i].stage != expected[i].stage || s_steps[i].percent != expected[i].percent) {
            fprintf(stderr, "progress step %zu: %d/%u, expected %d/%u\n", i, s_steps[i].stage, s_steps[i].percent,
                    expected[i].stage, expected[i].percent);
            exit(1);
        }
    }
}
#define CHECK_STEPS(...)                                                  \
    do {                                                                  \
        const step_t expected_[] = {__VA_ARGS__};                         \
        check_steps(expected_, sizeof(expected_) / sizeof(expected_[0])); \
    } while (0)

static int64_t s_elapsed_ms;

static xiaotai_err_t run(const xiaotai_network_t *network, bool manual_retry)
{
    s_step_count = 0;
    xiaotai_coprocessor_host_journal_clear();
    const int64_t start = xiaotai_time_us();
    const xiaotai_err_t err = network->prepare(network->self, manual_retry, progress, &s_progress_ctx);
    s_elapsed_ms = (xiaotai_time_us() - start) / 1000;
    return err;
}

static xiaotai_coprocessor_state_t state_of(const xiaotai_network_t *network)
{
    xiaotai_coprocessor_state_t state;
    xiaotai_coprocessor_state(network, &state);
    return state;
}

static void check_outcome(const xiaotai_network_t *network, xiaotai_network_stage_t stage, uint8_t rule,
                          xiaotai_err_t error, int line)
{
    const xiaotai_coprocessor_state_t state = state_of(network);
    if (state.stage != stage || state.rule != rule || state.error != error) {
        fprintf(stderr, "line %d: state stage %d rule %u error 0x%x, expected stage %d rule %u error 0x%x\n", line,
                state.stage, state.rule, (unsigned)state.error, stage, rule, (unsigned)error);
        exit(1);
    }
}
#define CHECK_OUTCOME(network, stage, rule, error) check_outcome(network, stage, rule, error, __LINE__)

/* ----------------------------------------------------------------- images */

#define IMAGE_BYTES 2600U
#define CHIP_UPDATER 0x0dU
#define CHIP_OTHER 0x14U
#define PROJECT "eh_cp_wifi_sta"
#define TARGET_VERSION "3.0.7-xiaotai.5"

static uint8_t s_image[IMAGE_BYTES];
static char s_image_sha[65];

static void hex(const uint8_t *bytes, size_t count, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < count; ++i) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 15];
    }
    out[2 * count] = '\0';
}

static void digest_hex(const uint8_t *data, size_t size, char out[65])
{
    uint8_t digest[32];
    CHECK(xiaotai_sha256(data, size, digest) == XIAOTAI_OK);
    hex(digest, 32, out);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}

static void target_elf(uint8_t out[32])
{
    for (unsigned i = 0; i < 32; ++i) out[i] = (uint8_t)(0xa0 + i);
}

/* Image header, first segment header and descriptor at the offsets of the
 * boot loader's format (checked against the deployed image's bytes). */
static void build_image(uint8_t *image, size_t size, uint16_t chip, const char *project, const char *version)
{
    for (size_t i = 0; i < size; ++i) image[i] = (uint8_t)(i * 7U + 3U);
    memset(image, 0, XIAOTAI_COPROCESSOR_IMAGE_MIN_BYTES);
    image[0] = 0xE9;
    image[1] = 4;
    put16(image + 12, chip);
    put16(image + 15, 0);
    put16(image + 17, 99);
    put32(image + 24, 0x42000020);
    put32(image + 28, (uint32_t)(size - 32U));
    uint8_t *app = image + 32;
    put32(app, 0xABCD5432UL);
    strcpy((char *)app + 16, version);
    strcpy((char *)app + 48, project);
    strcpy((char *)app + 112, "v5.5.4");
    target_elf(app + 144);
}

/* ---------------------------------------------------------- configurations */

static const char *const updater_protocols[] = {"3.0.7", NULL};
static const char *const reader_protocols[] = {"2.12.13", "3.0.7", NULL};
static char s_source_elf[3][65];
static xiaotai_coprocessor_source_t s_sources[3];
static xiaotai_coprocessor_image_t s_image_config;
static xiaotai_coprocessor_config_t s_updater;

static void source_elf(unsigned index, uint8_t out[32])
{
    for (unsigned i = 0; i < 32; ++i) out[i] = (uint8_t)(0x10 * (index + 1) + i);
}

static void setup_updater(void)
{
    static const char *const versions[] = {"3.0.7-xiaotai.2", "3.0.7-xiaotai.3", "3.0.7-xiaotai.4"};
    build_image(s_image, IMAGE_BYTES, CHIP_UPDATER, PROJECT, TARGET_VERSION);
    digest_hex(s_image, IMAGE_BYTES, s_image_sha);
    s_image_config = (xiaotai_coprocessor_image_t){s_image, IMAGE_BYTES, s_image_sha, PROJECT, TARGET_VERSION};
    for (unsigned i = 0; i < 3; ++i) {
        uint8_t elf[32];
        source_elf(i, elf);
        hex(elf, 32, s_source_elf[i]);
        s_sources[i] = (xiaotai_coprocessor_source_t){PROJECT, versions[i], s_source_elf[i], 0x10f760};
    }
    s_updater = (xiaotai_coprocessor_config_t){
        .provisioning = XIAOTAI_PROVISION_PORTAL | XIAOTAI_PROVISION_SERIAL,
        .chip_id = CHIP_UPDATER,
        .protocols = updater_protocols,
        .image = &s_image_config,
        .sources = s_sources,
        .source_count = 3,
        .legacy = {.project = PROJECT, .version = "1"},
        .legacy_protocol_capability = 0x0d,
        .legacy_image_max = 0x180000,
        .attempt_space = "c6_update",
        .attempt_key = "attempt",
    };
}

static const xiaotai_network_wifi_t s_reader_wifi = {.tx_buffers_on_demand = true};
static const char *const reader_commands[] = {"INFO", "RXRESET", "SINK0", "SINK1", "ROLLBACK", NULL};
static const char *const reader_restart[] = {"ROLLBACK", NULL};
static const char *const reader_projects[] = {"network_adapter", NULL};
static const xiaotai_coprocessor_config_t s_reader = {
    .provisioning = XIAOTAI_PROVISION_PORTAL | XIAOTAI_PROVISION_SERIAL,
    .wifi = &s_reader_wifi,
    .chip_id = CHIP_OTHER,
    .chip_revision = 100,
    .manual_link = true,
    .protocols = reader_protocols,
    .update_projects = reader_projects,
    .update_image_max = 0x180000,
    .update_block_bytes = 512,
    .update_restart_wait_ms = 3000,
    .command_channel = 0x57545331,
    .commands = reader_commands,
    .restart_commands = reader_restart,
    .restart_command_wait_ms = 4000,
};

static const xiaotai_network_t *boot(const xiaotai_coprocessor_config_t *config)
{
    const xiaotai_network_t *network = NULL;
    CHECK(xiaotai_coprocessor_create(config, &network) == XIAOTAI_OK && network);
    return network;
}

/* The coprocessor runs a firmware with a full descriptor. */
static void runs(const char *project, const char *version, const uint8_t elf[32])
{
    H->app_magic = 0xABCD5432UL;
    memset(H->project, 0, sizeof(H->project));
    memset(H->version, 0, sizeof(H->version));
    memset(H->platform, 0, sizeof(H->platform));
    strcpy(H->project, project);
    strcpy(H->version, version);
    strcpy(H->platform, "v5.5.4");
    memcpy(H->elf_sha256, elf, 32);
}

/* An updater board at power-on: the coprocessor answers on protocol 3.0.7 and
 * runs the first approved source; no marker. */
static void fresh_updater_peer(void)
{
    xiaotai_coprocessor_host_reset();
    H->chip = CHIP_UPDATER;
    H->major = 3;
    H->minor = 0;
    H->patch = 7;
    uint8_t elf[32];
    source_elf(0, elf);
    runs(PROJECT, "3.0.7-xiaotai.2", elf);
    H->image = s_image;
    marker(false);
    s_get_result = s_set_result = s_flush_result = s_erase_result = XIAOTAI_OK;
    s_kv_writes = 0;
}

static void runs_target(void)
{
    uint8_t elf[32];
    target_elf(elf);
    runs(PROJECT, TARGET_VERSION, elf);
}

/* The coprocessor of the pre-versioned protocol: no version, no descriptor,
 * the handshake's capability word, the known probe answer. */
static void legacy_peer(void)
{
    fresh_updater_peer();
    H->version_result = XIAOTAI_ERR_TIMEOUT;
    H->app_result = XIAOTAI_ERR_TIMEOUT;
    H->major = H->minor = H->patch = 0;
    H->capabilities = 0x0d;
    H->probe_matched = true;
}

#define PREFIX_UPDATER "wait 15000\nchip\nversion\napp\nkv get c6_update/attempt 3000\n"

/* ---------------------------------------------------------------- tests */

static void test_sha256_reference(void)
{
    char text[65];
    digest_hex((const uint8_t *)"abc", 3, text);
    CHECK_TEXT(text, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    digest_hex((const uint8_t *)"", 0, text);
    CHECK_TEXT(text, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    digest_hex((const uint8_t *)two, strlen(two), text);
    CHECK_TEXT(text, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

static void test_image_read(void)
{
    xiaotai_coprocessor_image_info_t info;
    CHECK(xiaotai_coprocessor_image_read(s_image, XIAOTAI_COPROCESSOR_IMAGE_MIN_BYTES - 1, &info) ==
          XIAOTAI_ERR_INVALID_SIZE);
    CHECK(xiaotai_coprocessor_image_read(s_image, IMAGE_BYTES, &info) == XIAOTAI_OK);
    uint8_t elf[32];
    target_elf(elf);
    CHECK(info.magic == 0xE9 && info.segment_count == 4 && info.chip_id == CHIP_UPDATER);
    CHECK(info.min_revision == 0 && info.max_revision == 99 && info.first_segment_bytes == IMAGE_BYTES - 32);
    CHECK(info.app.magic == 0xABCD5432UL && strcmp(info.app.project, PROJECT) == 0);
    CHECK(strcmp(info.app.version, TARGET_VERSION) == 0 && strcmp(info.app.platform, "v5.5.4") == 0);
    CHECK(memcmp(info.app.elf_sha256, elf, 32) == 0);
    uint8_t digest[32];
    CHECK(xiaotai_coprocessor_hex_digest(s_source_elf[1], digest) && digest[0] == 0x20 && digest[31] == 0x3f);
    CHECK(!xiaotai_coprocessor_hex_digest("12", digest));
    char odd[65];
    memcpy(odd, s_source_elf[1], sizeof(odd));
    odd[10] = 'g';
    CHECK(!xiaotai_coprocessor_hex_digest(odd, digest));
}

static void test_create(void)
{
    static const xiaotai_network_t sentinel;
    const xiaotai_network_t *network = &sentinel;
    CHECK(xiaotai_coprocessor_create(NULL, &network) == XIAOTAI_ERR_INVALID_ARG && network == NULL);
    CHECK(xiaotai_coprocessor_create(&s_reader, NULL) == XIAOTAI_ERR_INVALID_ARG);

    /* A restart command must be one of the commands. */
    static const char *const stray[] = {"REBOOT", NULL};
    xiaotai_coprocessor_config_t bad = s_reader;
    bad.restart_commands = stray;
    CHECK(xiaotai_coprocessor_create(&bad, &network) == XIAOTAI_ERR_INVALID_ARG && network == NULL);
    bad = s_reader;
    bad.commands = NULL;
    CHECK(xiaotai_coprocessor_create(&bad, &network) == XIAOTAI_ERR_INVALID_ARG);

    /* An updater needs a digest it can read and a place for its marker. */
    static xiaotai_coprocessor_image_t short_digest;
    short_digest = s_image_config;
    short_digest.sha256 = "8700a7";
    bad = s_updater;
    bad.image = &short_digest;
    CHECK(xiaotai_coprocessor_create(&bad, &network) == XIAOTAI_ERR_INVALID_ARG);
    bad = s_updater;
    bad.attempt_key = NULL;
    CHECK(xiaotai_coprocessor_create(&bad, &network) == XIAOTAI_ERR_INVALID_ARG);
    bad = s_updater;
    bad.attempt_space = "a_namespace_of_16";
    CHECK(xiaotai_coprocessor_create(&bad, &network) == XIAOTAI_ERR_INVALID_ARG);
    static xiaotai_coprocessor_source_t bad_source[1];
    bad_source[0] = s_sources[0];
    bad_source[0].elf_sha256 = "not hex";
    bad = s_updater;
    bad.sources = bad_source;
    bad.source_count = 1;
    CHECK(xiaotai_coprocessor_create(&bad, &network) == XIAOTAI_ERR_INVALID_ARG);

    /* One allocation; its failure is reported and leaves nothing behind. */
    xiaotai_mem_host_clear();
    xiaotai_mem_host_fail_nth(1);
    CHECK(xiaotai_coprocessor_create(&s_reader, &network) == XIAOTAI_ERR_NO_MEM && network == NULL);
    CHECK(xiaotai_mem_host_outstanding() == 0);
    xiaotai_mem_host_clear();
    network = boot(&s_reader);
    CHECK(xiaotai_mem_host_allocations() == 1 && xiaotai_mem_host_outstanding() == 1);

    CHECK(network->desc.kind == XIAOTAI_NETWORK_WIFI_COPROCESSOR);
    CHECK(network->desc.provisioning == (XIAOTAI_PROVISION_PORTAL | XIAOTAI_PROVISION_SERIAL));
    CHECK(network->driver_config == &s_reader_wifi);
    CHECK(network->prepare && network->coprocessor_info && network->self);
    CHECK(boot(&s_updater)->driver_config == NULL);

    xiaotai_coprocessor_state_t state = state_of(network);
    CHECK(state.rule == 0 && state.stage == XIAOTAI_NETWORK_CHECKING && !state.transfer_started);
    /* A network part that is not this part's reads as never prepared. */
    xiaotai_network_t foreign = *network;
    foreign.prepare = NULL;
    memset(&state, 0xff, sizeof(state));
    xiaotai_coprocessor_state(&foreign, &state);
    CHECK(state.rule == 0 && state.stage == 0 && !state.attempt_recorded);
    xiaotai_coprocessor_state(NULL, &state);
    CHECK(state.rule == 0);
}

/* Read-only check: handshake over a link prepare() starts, chip, protocol. */
static void test_read_only(void)
{
    xiaotai_coprocessor_host_reset();
    H->chip = CHIP_OTHER;
    H->major = 2;
    H->minor = 12;
    H->patch = 13;
    s_kv_writes = 0;
    const xiaotai_network_t *network = boot(&s_reader);
    CHECK(run(network, false) == XIAOTAI_OK);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(), "start 15000\nchip\nversion\n");
    CHECK_STEPS({XIAOTAI_NETWORK_CHECKING, 0}, {XIAOTAI_NETWORK_VERIFYING, 0}, {XIAOTAI_NETWORK_READY, 100});
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_READY, 5, XIAOTAI_OK);
    xiaotai_coprocessor_info_t info;
    CHECK(network->coprocessor_info(network->self, false, &info) == XIAOTAI_ERR_NOT_FOUND);

    H->major = 3;
    H->minor = 0;
    H->patch = 7;
    CHECK(run(network, false) == XIAOTAI_OK);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_READY, 5, XIAOTAI_OK);

    H->patch = 8;
    CHECK(run(network, false) == XIAOTAI_ERR_INVALID_VERSION);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_UNSUPPORTED, 3, XIAOTAI_ERR_INVALID_VERSION);
    CHECK_STEPS({XIAOTAI_NETWORK_CHECKING, 0}, {XIAOTAI_NETWORK_VERIFYING, 0}, {XIAOTAI_NETWORK_UNSUPPORTED, 0});

    H->patch = 7;
    H->version_result = XIAOTAI_ERR_TIMEOUT;
    CHECK(run(network, false) == XIAOTAI_ERR_TIMEOUT);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_UNSUPPORTED, 3, XIAOTAI_ERR_TIMEOUT);

    H->version_result = XIAOTAI_OK;
    H->chip = CHIP_UPDATER;
    CHECK(run(network, false) == XIAOTAI_ERR_INVALID_RESPONSE);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(), "start 15000\nchip\n");
    CHECK_STEPS({XIAOTAI_NETWORK_CHECKING, 0}, {XIAOTAI_NETWORK_UNSUPPORTED, 0});
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_UNSUPPORTED, 3, XIAOTAI_ERR_INVALID_RESPONSE);

    /* The read-only check never writes, never asks for the descriptor. */
    CHECK(s_kv_writes == 0 && H->apps == 0 && H->begins == 0);
}

/* Bring-up: link timeout, start failure, a manual retry stops before it starts. */
static void test_bring_up(void)
{
    xiaotai_coprocessor_host_reset();
    H->chip = CHIP_OTHER;
    H->major = 3;
    H->patch = 7;
    const xiaotai_network_t *network = boot(&s_reader);
    H->start_result = XIAOTAI_FAIL;
    CHECK(run(network, false) == XIAOTAI_FAIL);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(), "start 15000\n");
    CHECK_STEPS({XIAOTAI_NETWORK_CHECKING, 0}, {XIAOTAI_NETWORK_NO_LINK, 0});
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_NO_LINK, 1, XIAOTAI_FAIL);

    H->start_result = XIAOTAI_OK;
    CHECK(run(network, true) == XIAOTAI_OK);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(), "stop\nstart 15000\nchip\nversion\n");
    CHECK(H->stops == 1 && H->starts == 2);

    H->stop_result = XIAOTAI_FAIL;
    CHECK(run(network, true) == XIAOTAI_FAIL);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(), "stop\n");
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_NO_LINK, 1, XIAOTAI_FAIL);

    xiaotai_coprocessor_config_t timed = s_reader;
    timed.link_timeout_ms = 2500;
    H->stop_result = XIAOTAI_OK;
    CHECK(run(boot(&timed), false) == XIAOTAI_OK);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(), "start 2500\nchip\nversion\n");

    /* A link the system starts is only waited for, also on a retry. */
    fresh_updater_peer();
    H->wait_result = XIAOTAI_ERR_TIMEOUT;
    network = boot(&s_updater);
    CHECK(run(network, true) == XIAOTAI_ERR_TIMEOUT);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(), "wait 15000\n");
    CHECK_STEPS({XIAOTAI_NETWORK_CHECKING, 0}, {XIAOTAI_NETWORK_NO_LINK, 0});
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_NO_LINK, 1, XIAOTAI_ERR_TIMEOUT);
    CHECK(H->stops == 0 && H->starts == 0 && s_kv_writes == 0);
}

static void test_info(void)
{
    xiaotai_coprocessor_host_reset();
    H->chip = CHIP_OTHER;
    H->major = 2;
    H->minor = 12;
    H->patch = 13;
    uint8_t elf[32];
    for (unsigned i = 0; i < 32; ++i) elf[i] = (uint8_t)(0xf0 ^ i);
    runs("network_adapter", "2.12.13-vendor", elf);
    const xiaotai_network_t *network = boot(&s_reader);
    xiaotai_coprocessor_info_t info;
    CHECK(network->coprocessor_info(network->self, true, NULL) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(network->coprocessor_info(network->self, true, &info) == XIAOTAI_OK);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(), "version\napp\n");
    CHECK_TEXT(info.project, "network_adapter");
    CHECK_TEXT(info.version, "2.12.13-vendor");
    CHECK_TEXT(info.platform, "v5.5.4");
    CHECK_TEXT(info.protocol, "2.12.13");
    CHECK_TEXT(info.elf_sha256, "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeffe0e1e2e3e4e5e6e7e8e9eaebecedeeef");

    /* A field without its terminator is cut to what the info holds. */
    memset(H->project, 'p', sizeof(H->project));
    CHECK(network->coprocessor_info(network->self, true, &info) == XIAOTAI_OK);
    CHECK(strlen(info.project) == XIAOTAI_COPROCESSOR_TEXT_MAX - 1 && info.project[0] == 'p');

    H->app_result = XIAOTAI_ERR_TIMEOUT;
    CHECK(network->coprocessor_info(network->self, true, &info) == XIAOTAI_ERR_TIMEOUT);
    H->version_result = XIAOTAI_ERR_NOT_SUPPORTED;
    xiaotai_coprocessor_host_journal_clear();
    CHECK(network->coprocessor_info(network->self, true, &info) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(), "version\n");
    CHECK(network->coprocessor_info(network->self, false, &info) == XIAOTAI_ERR_NOT_FOUND);
}

/* Rule 4, and the read-back after an activation on the next boot. */
static void test_rule4_target_running(void)
{
    fresh_updater_peer();
    runs_target();
    marker(true);
    const xiaotai_network_t *network = boot(&s_updater);
    CHECK(run(network, false) == XIAOTAI_OK);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(), PREFIX_UPDATER "kv erase c6_update/attempt 3000\n");
    CHECK_STEPS({XIAOTAI_NETWORK_CHECKING, 0}, {XIAOTAI_NETWORK_VERIFYING, 1}, {XIAOTAI_NETWORK_READY, 100});
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_READY, 4, XIAOTAI_OK);
    CHECK(!s_marker_present && !state_of(network).attempt_recorded && H->begins == 0);

    xiaotai_coprocessor_info_t info;
    CHECK(network->coprocessor_info(network->self, false, &info) == XIAOTAI_OK);
    CHECK(H->versions == 1 && H->apps == 1); /* cached: no new request */
    CHECK_TEXT(info.project, PROJECT);
    CHECK_TEXT(info.version, TARGET_VERSION);
    CHECK_TEXT(info.platform, "v5.5.4");
    CHECK_TEXT(info.protocol, "3.0.7");
    CHECK_TEXT(info.elf_sha256, "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf");

    /* Without a marker nothing is written. */
    s_kv_writes = 0;
    CHECK(run(network, false) == XIAOTAI_OK);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(), PREFIX_UPDATER);
    CHECK(s_kv_writes == 0);

    /* Clearing the marker fails: not ready, retry. */
    marker(true);
    s_erase_result = XIAOTAI_ERR_TIMEOUT;
    CHECK(run(network, false) == XIAOTAI_ERR_TIMEOUT);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_NO_LINK, 4, XIAOTAI_ERR_TIMEOUT);
    s_erase_result = XIAOTAI_OK;

    /* The same firmware on a protocol the board does not accept. */
    marker(false);
    H->patch = 8;
    CHECK(run(network, false) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_UNSUPPORTED, 3, XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(H->begins == 0);

    /* An incomplete descriptor is not cached. */
    H->patch = 7;
    memset(H->platform, 0, sizeof(H->platform));
    CHECK(run(network, false) == XIAOTAI_OK);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_READY, 4, XIAOTAI_OK);
    CHECK(network->coprocessor_info(network->self, false, &info) == XIAOTAI_ERR_NOT_FOUND);
}

static void test_updater_cached_identity_boundaries(void)
{
    fresh_updater_peer();
    runs_target();
    const xiaotai_network_t *network = boot(&s_updater);
    xiaotai_coprocessor_info_t info;
    CHECK(network->coprocessor_info(network->self, false, &info) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(!H->versions && !H->apps);

    CHECK(run(network, false) == XIAOTAI_OK);
    strcpy(H->platform, "next");
    CHECK(network->coprocessor_info(network->self, false, &info) == XIAOTAI_OK);
    CHECK_TEXT(info.platform, "v5.5.4");
    CHECK(H->versions == 1 && H->apps == 1);

    memset(H->platform, 'X', sizeof(H->platform));
    CHECK(run(network, false) == XIAOTAI_OK);
    CHECK(network->coprocessor_info(network->self, false, &info) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(H->versions == 2 && H->apps == 2);

    runs_target();
    H->app_magic = 0;
    CHECK(run(network, false) == XIAOTAI_OK);
    CHECK(network->coprocessor_info(network->self, false, &info) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(H->versions == 3 && H->apps == 3);

    runs_target();
    memset(H->elf_sha256, 0, sizeof(H->elf_sha256));
    CHECK(run(network, false) == XIAOTAI_OK);
    CHECK(network->coprocessor_info(network->self, false, &info) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(H->versions == 4 && H->apps == 4);
    CHECK(s_kv_writes == 0 && H->begins == 0);
}

/* Rule 10 and 11: firmware that is no update source. */
static void test_unknown_firmware(void)
{
    fresh_updater_peer();
    H->elf_sha256[0] ^= 1;
    const xiaotai_network_t *network = boot(&s_updater);
    CHECK(run(network, false) == XIAOTAI_OK);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(), PREFIX_UPDATER);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_READY, 10, XIAOTAI_OK);
    CHECK(s_kv_writes == 0);

    /* A recorded marker: no rule may update it, manual retry or not. */
    marker(true);
    CHECK(run(network, true) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_UNSUPPORTED, 11, XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(run(network, false) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_UNSUPPORTED, 11, XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(H->begins == 0 && s_marker_present && s_kv_writes == 0);

    /* No magic and an unknown name is left alone too; an empty version is not. */
    marker(false);
    H->app_magic = 0;
    strcpy(H->version, "2");
    CHECK(run(network, false) == XIAOTAI_OK);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_READY, 10, XIAOTAI_OK);
    H->app_magic = 0xABCD5432UL;
    memset(H->version, 0, sizeof(H->version));
    CHECK(run(network, false) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_UNSUPPORTED, 11, XIAOTAI_ERR_NOT_SUPPORTED);

    /* An approved source whose verified limit is below the image size. */
    fresh_updater_peer();
    static xiaotai_coprocessor_source_t small[3];
    memcpy(small, s_sources, sizeof(small));
    small[0].image_max = IMAGE_BYTES - 1;
    xiaotai_coprocessor_config_t limited = s_updater;
    limited.sources = small;
    network = boot(&limited);
    CHECK(run(network, false) == XIAOTAI_OK);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_READY, 10, XIAOTAI_OK);
    marker(true);
    CHECK(run(network, true) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(H->begins == 0);

    /* A source without an ELF digest takes any build of its version. */
    fresh_updater_peer();
    H->elf_sha256[5] ^= 0x40;
    marker(true);
    static xiaotai_coprocessor_source_t any_build[1];
    any_build[0] = s_sources[0];
    any_build[0].elf_sha256 = NULL;
    xiaotai_coprocessor_config_t open = s_updater;
    open.sources = any_build;
    open.source_count = 1;
    network = boot(&open);
    CHECK(run(network, false) == XIAOTAI_ERR_INVALID_STATE);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_UNFINISHED, 7, XIAOTAI_ERR_INVALID_STATE);
}

/* Rule 6: the older firmware without a magic, known to work. */
static void test_legacy_firmware(void)
{
    fresh_updater_peer();
    H->app_magic = 0;
    memset(H->version, 0, sizeof(H->version));
    strcpy(H->version, "1");
    memset(H->elf_sha256, 0, sizeof(H->elf_sha256));
    const xiaotai_network_t *network = boot(&s_updater);
    CHECK(run(network, false) == XIAOTAI_OK);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_READY, 6, XIAOTAI_OK);
    CHECK(s_kv_writes == 0 && H->begins == 0);
    xiaotai_coprocessor_info_t info;
    CHECK(network->coprocessor_info(network->self, false, &info) == XIAOTAI_ERR_NOT_FOUND);

    /* With a marker it is neither left alone nor an update source. */
    marker(true);
    CHECK(run(network, false) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_UNSUPPORTED, 11, XIAOTAI_ERR_NOT_SUPPORTED);
}

/* Rules 7 and 8: the marker holds back the update until a manual retry; the
 * marker is durable before the first block; the next boot reads back. */
static void test_update(void)
{
    fresh_updater_peer();
    marker(true);
    const xiaotai_network_t *network = boot(&s_updater);
    CHECK(run(network, false) == XIAOTAI_ERR_INVALID_STATE);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(), PREFIX_UPDATER);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_UNFINISHED, 7, XIAOTAI_ERR_INVALID_STATE);
    CHECK(state_of(network).attempt_recorded && H->begins == 0 && s_kv_writes == 0);

    CHECK(run(network, true) == XIAOTAI_OK);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(),
               PREFIX_UPDATER "kv set c6_update/attempt=1 3000\nkv flush 3000\nbegin\n"
                              "write +0 1024\nwrite +1024 1024\nwrite +2048 552\nend\nactivate\n");
    CHECK_STEPS({XIAOTAI_NETWORK_CHECKING, 0}, {XIAOTAI_NETWORK_VERIFYING, 1}, {XIAOTAI_NETWORK_WRITING, 5},
                {XIAOTAI_NETWORK_WRITING, 38}, {XIAOTAI_NETWORK_WRITING, 71}, {XIAOTAI_NETWORK_WRITING, 90},
                {XIAOTAI_NETWORK_ACTIVATING, 94}, {XIAOTAI_NETWORK_RESTARTING, 96});
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_RESTARTING, 8, XIAOTAI_OK);
    CHECK(H->contiguous && H->written == IMAGE_BYTES && H->largest_write == 1024);
    /* Default wait after activation: 50 ms (and nowhere near the legacy wait). */
    CHECK(s_elapsed_ms >= 50 && s_elapsed_ms < 3000);
    xiaotai_coprocessor_state_t state = state_of(network);
    CHECK(state.transfer_started && state.attempt_recorded && !state.legacy_protocol);
    CHECK(s_marker_present && s_marker == 1);
    xiaotai_coprocessor_info_t info;
    CHECK(network->coprocessor_info(network->self, false, &info) == XIAOTAI_ERR_NOT_FOUND);

    /* One transfer per boot, even if nobody restarted. */
    CHECK(run(network, true) == XIAOTAI_ERR_INVALID_STATE);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_POWER_CYCLE, 8, XIAOTAI_ERR_INVALID_STATE);
    CHECK(H->begins == 1);

    /* Next boot: the coprocessor runs the image; rule 4 confirms it. */
    runs_target();
    network = boot(&s_updater);
    CHECK(run(network, false) == XIAOTAI_OK);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(), PREFIX_UPDATER "kv erase c6_update/attempt 3000\n");
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_READY, 4, XIAOTAI_OK);
    CHECK(!s_marker_present && H->begins == 1);

    /* No marker: an approved source is updated at once; the block size is
     * the board's. */
    fresh_updater_peer();
    xiaotai_coprocessor_config_t blocks = s_updater;
    blocks.write_block_bytes = 1300;
    blocks.activate_wait_ms = 120;
    network = boot(&blocks);
    CHECK(run(network, false) == XIAOTAI_OK);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(),
               PREFIX_UPDATER "kv set c6_update/attempt=1 3000\nkv flush 3000\nbegin\n"
                              "write +0 1300\nwrite +1300 1300\nend\nactivate\n");
    CHECK(s_elapsed_ms >= 120);
}

/* A failed block is never resent and the transfer is not ended. */
static void test_transfer_failures(void)
{
    fresh_updater_peer();
    H->fail_write = 2;
    H->write_error = XIAOTAI_ERR_TIMEOUT;
    const xiaotai_network_t *network = boot(&s_updater);
    CHECK(run(network, false) == XIAOTAI_ERR_TIMEOUT);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(),
               PREFIX_UPDATER "kv set c6_update/attempt=1 3000\nkv flush 3000\nbegin\n"
                              "write +0 1024\nwrite +1024 1024\n");
    CHECK_STEPS({XIAOTAI_NETWORK_CHECKING, 0}, {XIAOTAI_NETWORK_VERIFYING, 1}, {XIAOTAI_NETWORK_WRITING, 5},
                {XIAOTAI_NETWORK_WRITING, 38}, {XIAOTAI_NETWORK_POWER_CYCLE, 0});
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_POWER_CYCLE, 8, XIAOTAI_ERR_TIMEOUT);
    CHECK(H->written == 1024 && H->ends == 0 && H->activations == 0 && s_marker == 1);

    /* A retry in the same boot transfers nothing; the identity is read again. */
    CHECK(run(network, true) == XIAOTAI_ERR_INVALID_STATE);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_POWER_CYCLE, 8, XIAOTAI_ERR_INVALID_STATE);
    CHECK(H->begins == 1 && H->writes == 2);
    xiaotai_coprocessor_info_t info;
    CHECK(network->coprocessor_info(network->self, false, &info) == XIAOTAI_OK);
    CHECK_TEXT(info.version, "3.0.7-xiaotai.2");

    /* Every failure after the transfer asks for a power cycle ... */
    H->app_result = XIAOTAI_ERR_TIMEOUT;
    CHECK(run(network, true) == XIAOTAI_ERR_TIMEOUT);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_POWER_CYCLE, 1, XIAOTAI_ERR_TIMEOUT);
    /* ... except a link that does not come up. */
    H->wait_result = XIAOTAI_ERR_TIMEOUT;
    CHECK(run(network, true) == XIAOTAI_ERR_TIMEOUT);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_NO_LINK, 1, XIAOTAI_ERR_TIMEOUT);

    /* begin fails: the marker is already there. */
    fresh_updater_peer();
    H->begin_result = XIAOTAI_FAIL;
    network = boot(&s_updater);
    CHECK(run(network, false) == XIAOTAI_FAIL);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_POWER_CYCLE, 8, XIAOTAI_FAIL);
    CHECK(H->writes == 0 && s_marker == 1);

    /* end fails: no activation. */
    fresh_updater_peer();
    H->end_result = XIAOTAI_ERR_TIMEOUT;
    network = boot(&s_updater);
    CHECK(run(network, false) == XIAOTAI_ERR_TIMEOUT);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_POWER_CYCLE, 8, XIAOTAI_ERR_TIMEOUT);
    CHECK(H->ends == 1 && H->activations == 0 && s_marker == 1);

    /* activate fails: it may have started, so the host restarts anyway. */
    fresh_updater_peer();
    H->activate_result = XIAOTAI_ERR_TIMEOUT;
    network = boot(&s_updater);
    CHECK(run(network, false) == XIAOTAI_OK);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_RESTARTING, 8, XIAOTAI_ERR_TIMEOUT);
    CHECK(H->activations == 1 && s_marker == 1);
}

/* The marker store fails: nothing is transferred. */
static void test_marker_failures(void)
{
    fresh_updater_peer();
    s_get_result = XIAOTAI_ERR_TIMEOUT;
    const xiaotai_network_t *network = boot(&s_updater);
    CHECK(run(network, false) == XIAOTAI_ERR_TIMEOUT);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_NO_LINK, 7, XIAOTAI_ERR_TIMEOUT);
    CHECK(H->begins == 0);

    fresh_updater_peer();
    s_set_result = XIAOTAI_ERR_NO_MEM;
    network = boot(&s_updater);
    CHECK(run(network, false) == XIAOTAI_ERR_NO_MEM);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(), PREFIX_UPDATER "kv set c6_update/attempt=1 3000\n");
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_NO_LINK, 8, XIAOTAI_ERR_NO_MEM);
    CHECK(!state_of(network).transfer_started && H->begins == 0);
    /* Nothing started, so a retry may still transfer. */
    s_set_result = XIAOTAI_OK;
    CHECK(run(network, true) == XIAOTAI_OK && H->begins == 1);

    fresh_updater_peer();
    s_flush_result = XIAOTAI_ERR_TIMEOUT;
    network = boot(&s_updater);
    CHECK(run(network, false) == XIAOTAI_ERR_TIMEOUT);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(), PREFIX_UPDATER "kv set c6_update/attempt=1 3000\nkv flush 3000\n");
    CHECK(H->begins == 0);
}

/* Rule 9: the pre-versioned protocol. */
static void test_legacy_protocol(void)
{
    legacy_peer();
    H->end_result = XIAOTAI_ERR_TIMEOUT; /* end commits and restarts before it answers */
    const xiaotai_network_t *network = boot(&s_updater);
    CHECK(run(network, false) == XIAOTAI_OK);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(),
               "wait 15000\nchip\nversion\nfacts\nprobe\napp\nkv get c6_update/attempt 3000\n"
               "kv set c6_update/attempt=1 3000\nkv flush 3000\nbegin\n"
               "write +0 1024\nwrite +1024 1024\nwrite +2048 552\nend\n");
    CHECK_STEPS({XIAOTAI_NETWORK_CHECKING, 0}, {XIAOTAI_NETWORK_VERIFYING, 1}, {XIAOTAI_NETWORK_WRITING, 5},
                {XIAOTAI_NETWORK_WRITING, 38}, {XIAOTAI_NETWORK_WRITING, 71}, {XIAOTAI_NETWORK_WRITING, 90},
                {XIAOTAI_NETWORK_RESTARTING, 96});
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_RESTARTING, 9, XIAOTAI_ERR_TIMEOUT);
    CHECK(H->activations == 0 && state_of(network).legacy_protocol && s_marker == 1);
    /* The old firmware restarts itself 5 s after end: default wait 6000 ms. */
    CHECK(s_elapsed_ms >= 6000);

    /* The probe gets another answer: not that protocol. */
    legacy_peer();
    H->probe_matched = false;
    network = boot(&s_updater);
    CHECK(run(network, false) == XIAOTAI_ERR_TIMEOUT);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(), "wait 15000\nchip\nversion\nfacts\nprobe\n");
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_NO_LINK, 1, XIAOTAI_ERR_TIMEOUT);
    /* No answer at all is no evidence either. */
    H->probe_result = XIAOTAI_ERR_TIMEOUT;
    H->probe_matched = true;
    CHECK(run(network, false) == XIAOTAI_ERR_TIMEOUT && H->begins == 0);

    /* A handshake with anything newer skips the probe. */
    legacy_peer();
    H->capabilities = 0x0f;
    network = boot(&s_updater);
    CHECK(run(network, false) == XIAOTAI_ERR_TIMEOUT);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(), "wait 15000\nchip\nversion\nfacts\n");
    legacy_peer();
    H->rpc_version_advertised = true;
    CHECK(run(boot(&s_updater), false) == XIAOTAI_ERR_TIMEOUT && H->probes == 0);
    legacy_peer();
    H->bus_buffer_config = true;
    CHECK(run(boot(&s_updater), false) == XIAOTAI_ERR_TIMEOUT && H->probes == 0);
    legacy_peer();
    H->firmware = 0x01000000;
    CHECK(run(boot(&s_updater), false) == XIAOTAI_ERR_TIMEOUT && H->probes == 0);

    /* It answers the descriptor request: something else, partly broken. */
    legacy_peer();
    H->app_result = XIAOTAI_OK;
    network = boot(&s_updater);
    CHECK(run(network, false) == XIAOTAI_ERR_INVALID_RESPONSE);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_NO_LINK, 1, XIAOTAI_ERR_INVALID_RESPONSE);
    CHECK(s_kv_writes == 0 && H->begins == 0);

    /* The image must fit the old slot. */
    legacy_peer();
    xiaotai_coprocessor_config_t small = s_updater;
    small.legacy_image_max = IMAGE_BYTES - 1;
    network = boot(&small);
    CHECK(run(network, true) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_UNSUPPORTED, 11, XIAOTAI_ERR_NOT_SUPPORTED);

    /* A board without the legacy capability never probes. */
    legacy_peer();
    xiaotai_coprocessor_config_t none = s_updater;
    none.legacy_protocol_capability = 0;
    network = boot(&none);
    CHECK(run(network, false) == XIAOTAI_ERR_TIMEOUT);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(), "wait 15000\nchip\nversion\n");

    /* The marker holds back a legacy transfer too; the board's wait is used. */
    legacy_peer();
    marker(true);
    xiaotai_coprocessor_config_t quick = s_updater;
    quick.legacy_activate_wait_ms = 150;
    quick.activate_wait_ms = 5000;
    network = boot(&quick);
    CHECK(run(network, false) == XIAOTAI_ERR_INVALID_STATE);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_UNFINISHED, 7, XIAOTAI_ERR_INVALID_STATE);
    CHECK(run(network, true) == XIAOTAI_OK);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_RESTARTING, 9, XIAOTAI_OK);
    CHECK(s_elapsed_ms >= 150 && s_elapsed_ms < 5000);
    /* end activates on that protocol: activate is never sent after it. */
    CHECK(H->ends == 1 && H->activations == 0);
    CHECK_STEPS({XIAOTAI_NETWORK_CHECKING, 0}, {XIAOTAI_NETWORK_VERIFYING, 1}, {XIAOTAI_NETWORK_WRITING, 5},
                {XIAOTAI_NETWORK_WRITING, 38}, {XIAOTAI_NETWORK_WRITING, 71}, {XIAOTAI_NETWORK_WRITING, 90},
                {XIAOTAI_NETWORK_RESTARTING, 96});
}

/* Chip and descriptor answers of the updater. */
static void test_identity_answers(void)
{
    fresh_updater_peer();
    H->chip = CHIP_OTHER;
    const xiaotai_network_t *network = boot(&s_updater);
    CHECK(run(network, false) == XIAOTAI_ERR_INVALID_RESPONSE);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(), "wait 15000\nchip\n");
    CHECK_STEPS({XIAOTAI_NETWORK_CHECKING, 0}, {XIAOTAI_NETWORK_VERIFYING, 1}, {XIAOTAI_NETWORK_UNSUPPORTED, 0});
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_UNSUPPORTED, 3, XIAOTAI_ERR_INVALID_RESPONSE);

    H->chip = 0;
    CHECK(run(network, false) == XIAOTAI_ERR_INVALID_RESPONSE);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_NO_LINK, 1, XIAOTAI_ERR_INVALID_RESPONSE);

    fresh_updater_peer();
    H->app_result = XIAOTAI_ERR_NOT_SUPPORTED;
    network = boot(&s_updater);
    CHECK(run(network, false) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(), "wait 15000\nchip\nversion\napp\n");
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_NO_LINK, 1, XIAOTAI_ERR_NOT_SUPPORTED);

    fresh_updater_peer();
    memset(H->version, 'v', sizeof(H->version));
    CHECK(run(network, false) == XIAOTAI_ERR_INVALID_RESPONSE);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_NO_LINK, 1, XIAOTAI_ERR_INVALID_RESPONSE);
    CHECK(s_kv_writes == 0 && H->begins == 0);
}

/* Rule 2: the board's image is checked before the coprocessor is asked. */
static uint8_t s_bad[IMAGE_BYTES];
static char s_bad_sha[65];
static xiaotai_coprocessor_image_t s_bad_image;

static void bad_image_case(size_t size, uint16_t chip, uint8_t edit_offset, uint8_t edit_value, const char *project,
                           const char *version, uint16_t revision, xiaotai_err_t expected, int line)
{
    build_image(s_bad, size, chip, PROJECT, TARGET_VERSION);
    if (edit_offset) s_bad[edit_offset] = edit_value;
    digest_hex(s_bad, size, s_bad_sha);
    s_bad_image = (xiaotai_coprocessor_image_t){s_bad, size, s_bad_sha, project, version};
    static xiaotai_coprocessor_config_t config;
    config = s_updater;
    config.image = &s_bad_image;
    config.chip_revision = revision;
    fresh_updater_peer();
    const xiaotai_network_t *network = boot(&config);
    const xiaotai_err_t err = run(network, true);
    if (err != expected) {
        fprintf(stderr, "line %d: image case returned 0x%x, expected 0x%x\n", line, (unsigned)err, (unsigned)expected);
        exit(1);
    }
    if (expected == XIAOTAI_OK) return;
    CHECK_TEXT(xiaotai_coprocessor_host_journal(), "wait 15000\n");
    CHECK_STEPS({XIAOTAI_NETWORK_CHECKING, 0}, {XIAOTAI_NETWORK_VERIFYING, 1}, {XIAOTAI_NETWORK_BAD_IMAGE, 0});
    check_outcome(network, XIAOTAI_NETWORK_BAD_IMAGE, 2, expected, line);
    CHECK(s_kv_writes == 0 && H->begins == 0);
    xiaotai_coprocessor_info_t info;
    CHECK(network->coprocessor_info(network->self, false, &info) == XIAOTAI_ERR_NOT_FOUND);
}
#define BAD_IMAGE(...) bad_image_case(__VA_ARGS__, __LINE__)

static void test_bad_image(void)
{
    /* Digest of other bytes. */
    static xiaotai_coprocessor_image_t wrong_digest;
    wrong_digest = s_image_config;
    wrong_digest.sha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    xiaotai_coprocessor_config_t config = s_updater;
    config.image = &wrong_digest;
    fresh_updater_peer();
    const xiaotai_network_t *network = boot(&config);
    CHECK(run(network, false) == XIAOTAI_ERR_INVALID_CRC);
    CHECK_OUTCOME(network, XIAOTAI_NETWORK_BAD_IMAGE, 2, XIAOTAI_ERR_INVALID_CRC);
    CHECK_TEXT(xiaotai_coprocessor_host_journal(), "wait 15000\n");

    BAD_IMAGE(XIAOTAI_COPROCESSOR_IMAGE_MIN_BYTES - 1, CHIP_UPDATER, 0, 0, PROJECT, TARGET_VERSION, 0,
              XIAOTAI_ERR_INVALID_SIZE);
    BAD_IMAGE(IMAGE_BYTES, CHIP_UPDATER, 0, 0, PROJECT, TARGET_VERSION, 0, XIAOTAI_OK);  /* the control case */
    BAD_IMAGE(IMAGE_BYTES, CHIP_OTHER, 0, 0, PROJECT, TARGET_VERSION, 0, XIAOTAI_ERR_INVALID_RESPONSE);
    BAD_IMAGE(IMAGE_BYTES, CHIP_UPDATER, 1, 0, PROJECT, TARGET_VERSION, 0, XIAOTAI_ERR_INVALID_RESPONSE);  /* no segments */
    BAD_IMAGE(IMAGE_BYTES, CHIP_UPDATER, 32, 0x33, PROJECT, TARGET_VERSION, 0, XIAOTAI_ERR_INVALID_RESPONSE); /* descriptor magic */
    BAD_IMAGE(IMAGE_BYTES, CHIP_UPDATER, 0, 0, "other_project", TARGET_VERSION, 0, XIAOTAI_ERR_INVALID_RESPONSE);
    BAD_IMAGE(IMAGE_BYTES, CHIP_UPDATER, 0, 0, PROJECT, "3.0.7-xiaotai.6", 0, XIAOTAI_ERR_INVALID_RESPONSE);
    /* Revision range 0..99 does not include a required 1.0; it includes 0.5. */
    BAD_IMAGE(IMAGE_BYTES, CHIP_UPDATER, 0, 0, PROJECT, TARGET_VERSION, 100, XIAOTAI_ERR_INVALID_RESPONSE);
    BAD_IMAGE(IMAGE_BYTES, CHIP_UPDATER, 0, 0, PROJECT, TARGET_VERSION, 50, XIAOTAI_OK);
    /* Header magic: the digest still matches, the format does not. */
    build_image(s_bad, IMAGE_BYTES, CHIP_UPDATER, PROJECT, TARGET_VERSION);
    s_bad[0] = 0xE8;
    digest_hex(s_bad, IMAGE_BYTES, s_bad_sha);
    s_bad_image = (xiaotai_coprocessor_image_t){s_bad, IMAGE_BYTES, s_bad_sha, PROJECT, TARGET_VERSION};
    config = s_updater;
    config.image = &s_bad_image;
    fresh_updater_peer();
    CHECK(run(boot(&config), false) == XIAOTAI_ERR_INVALID_RESPONSE);
}

int main(void)
{
    H = xiaotai_coprocessor_host();
    setup_updater();
    test_sha256_reference();
    test_image_read();
    test_create();
    test_read_only();
    test_bring_up();
    test_info();
    test_rule4_target_running();
    test_updater_cached_identity_boundaries();
    test_unknown_firmware();
    test_legacy_firmware();
    test_update();
    test_transfer_failures();
    test_marker_failures();
    test_legacy_protocol();
    test_identity_answers();
    test_bad_image();
    puts("part_coprocessor: PASS");
    return 0;
}
