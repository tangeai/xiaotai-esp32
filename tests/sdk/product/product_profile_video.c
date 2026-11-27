// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_h5_video_send.py
#include "product_platform.h"
#include "product_test.h"
#include "xiaotai/json.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port/sys.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void product_profile_video_prepare(bool expect_h5, const char *board_model);
void product_profile_video_run(void);

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s video profile: %s:%d: %s\n", XIAOTAI_PRODUCT_NAME, __FILE__, __LINE__, #condition); \
    abort(); \
} } while (0)

static bool expect_video, override_model, reject_model;
static const char *expected_board;
static atomic_uint posts;
static atomic_size_t posted_bytes, document_capacity;

xiaotai_err_t __real_xiaotai_platform_configure(const xiaotai_platform_config_t *config);
xiaotai_err_t __wrap_xiaotai_platform_configure(const xiaotai_platform_config_t *config);
int __real_platform_profile_document(char *out, size_t capacity);
int __wrap_platform_profile_document(char *out, size_t capacity);

xiaotai_err_t __wrap_xiaotai_platform_configure(const xiaotai_platform_config_t *config)
{
    if (!override_model) return __real_xiaotai_platform_configure(config);
    /* A synthesized board identity value, with the real product selection,
     * report builder, slot sizing and platform transport left in place. */
    CHECK(config && config->board_model && config->chip_model);
    xiaotai_platform_config_t altered = *config;
    altered.board_model = expected_board;
    const xiaotai_err_t result = __real_xiaotai_platform_configure(&altered);
    if (reject_model) {
        /* This case ends at the product's first refusal, before its startup
         * loop handles a configuration error by requesting a restart. */
        CHECK(result == XIAOTAI_ERR_INVALID_ARG && !atomic_load(&posts) && !atomic_load(&document_capacity));
        printf("%s: platform configure refused model, result=%d POST count=0\n", XIAOTAI_PRODUCT_NAME, (int)result);
        exit(EXIT_SUCCESS);
    }
    CHECK(result == XIAOTAI_OK);
    return result;
}

int __wrap_platform_profile_document(char *out, size_t capacity)
{
    atomic_store(&document_capacity, capacity);
    return __real_platform_profile_document(out, capacity);
}

#if PRODUCT_TEST_PROFILE_VIDEO_ENCODER
#include "xiaotai/video_backend.h"

xiaotai_err_t __real_xiaotai_video_port_init(void);
xiaotai_err_t __wrap_xiaotai_video_port_init(void);

static xiaotai_video_encoder_config_t jpeg_config;

static int jpeg_match(const xiaotai_video_encoder_config_t *config)
{
    return config->codec == XIAOTAI_VIDEO_CODEC_JPEG && config->pixel == XIAOTAI_VIDEO_PIXEL_YUYV422 ? 1 : 0;
}

static xiaotai_err_t jpeg_open(const xiaotai_video_encoder_config_t *config, void **state)
{
    jpeg_config = *config;
    *state = &jpeg_config;
    return XIAOTAI_OK;
}

static xiaotai_err_t jpeg_encode(void *state, const xiaotai_video_image_t *image, bool key,
                                 xiaotai_video_packet_t *packet)
{
    static const uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xd9};
    CHECK(state == &jpeg_config && image->pixel == jpeg_config.pixel && key);
    CHECK(jpeg_config.output && jpeg_config.output_capacity >= sizeof(jpeg));
    memcpy(jpeg_config.output, jpeg, sizeof(jpeg));
    *packet = (xiaotai_video_packet_t){.data = jpeg_config.output, .size = sizeof(jpeg), .key_frame = true};
    return XIAOTAI_OK;
}

static void jpeg_close(void *state) { CHECK(state == &jpeg_config); }

xiaotai_err_t __wrap_xiaotai_video_port_init(void)
{
    const xiaotai_err_t error = __real_xiaotai_video_port_init();
    if (error != XIAOTAI_OK || !expect_video) return error;
    static const xiaotai_video_encoder_t fixture = {.name = "profile-jpeg", .load = XIAOTAI_VIDEO_LOAD_SOFTWARE,
        .match = jpeg_match, .open = jpeg_open, .encode = jpeg_encode, .close = jpeg_close};
    return xiaotai_video_encoder_register(&fixture);
}
#endif

static int field(const xiaotai_json_doc_t *doc, int object, const char *key)
{
    const int index = xiaotai_json_get(doc, object, key);
    if (index < 0) fprintf(stderr, "Missing video profile field: %s\n", key);
    CHECK(index >= 0);
    return index;
}

static void integer(const xiaotai_json_doc_t *doc, int object, const char *key, int64_t expected)
{
    int64_t value;
    CHECK(xiaotai_json_int(doc, field(doc, object, key), &value) == XIAOTAI_OK && value == expected);
}

static void boolean(const xiaotai_json_doc_t *doc, int object, const char *key, bool expected)
{
    bool value;
    CHECK(xiaotai_json_bool(doc, field(doc, object, key), &value) == XIAOTAI_OK && value == expected);
}

static void string(const xiaotai_json_doc_t *doc, int object, const char *key, const char *expected)
{
    CHECK(xiaotai_json_equals(doc, field(doc, object, key), expected));
}

static void codec_array(const xiaotai_json_doc_t *doc, int object, const char *key, const char *expected)
{
    const int array = field(doc, object, key);
    CHECK(doc->tokens[array].type == XIAOTAI_JSON_ARRAY);
    CHECK(doc->tokens[array].children == (expected ? 1 : 0));
    if (expected) CHECK(xiaotai_json_equals(doc, xiaotai_json_at(doc, array, 0), expected));
}

static void geometry(const xiaotai_json_doc_t *doc, int scene, const char *aspect)
{
    integer(doc, scene, "camera_rotation", 0);
    boolean(doc, scene, "hor_mirror", false);
    boolean(doc, scene, "vert_mirror", false);
    string(doc, scene, "aspect_ratio", aspect);
    string(doc, scene, "object_fit", "contain");
}

static void check_document(const xiaotai_http_host_request_t *request)
{
    CHECK(request->method == XIAOTAI_HTTP_POST);
    CHECK(!strcmp(request->url, "http://fixture.invalid/device/v1/device/profile"));
    CHECK(request->body && request->body_size && request->has_content_length && !request->streamed);
    unsigned auth = 0, json = 0;
    for (size_t i = 0; i < request->header_count; ++i) {
        const xiaotai_http_header_t *header = &request->headers[i];
        if (!strcmp(header->name, "Authorization")) {
            CHECK(!strcmp(header->value, "Bearer a.eyJleHAiOjE3MDAwMDA1MDB9.b"));
            ++auth;
        } else if (!strcmp(header->name, "Content-Type")) {
            CHECK(!strcmp(header->value, "application/json"));
            ++json;
        }
    }
    CHECK(auth == 1U && json == 1U);

    static xiaotai_json_token_t tokens[160]; /* This case expects one serialized HTTP exchange. */
    xiaotai_json_doc_t doc;
    size_t count;
    CHECK(xiaotai_json_parse((const char *)request->body, request->body_size, tokens,
                             sizeof(tokens) / sizeof(*tokens), &count, &doc) == XIAOTAI_OK);
    const int hardware = field(&doc, 0, "hardware");
    string(&doc, hardware, "chip_model", "ESP32-S3");
    string(&doc, hardware, "board_model", expected_board);
    xiaotai_firmware_t firmware;
    xiaotai_firmware(&firmware);
    string(&doc, 0, "firmware_version", firmware.version ? firmware.version : "unknown");

    const int profiles = field(&doc, 0, "profiles");
    const int stream = field(&doc, profiles, "stream");
    const int call = field(&doc, profiles, "call");
    const int voip = field(&doc, profiles, "voip");
    const int scenes[] = {stream, call, voip};
    for (size_t i = 0; i < sizeof(scenes) / sizeof(*scenes); ++i) {
        integer(&doc, scenes[i], "audio_rate", 8000);
        integer(&doc, scenes[i], "audio_channels", 1);
    }

    integer(&doc, stream, "up_audio_streamid", 10);
    integer(&doc, stream, "down_audio_streamid", 14);
    codec_array(&doc, stream, "up_audio_mt", "alaw");
    codec_array(&doc, stream, "down_audio_mt", "alaw");
    codec_array(&doc, stream, "up_video_mt", expect_video ? "mjpeg" : NULL);
    codec_array(&doc, stream, "down_video_mt", NULL);
    boolean(&doc, stream, "no_video", !expect_video);
    if (expect_video) {
        integer(&doc, stream, "up_video_streamid", 11);
        integer(&doc, stream, "down_video_streamid", 15);
        geometry(&doc, stream, "4:3");
    } else {
        CHECK(xiaotai_json_get(&doc, stream, "up_video_streamid") < 0);
        CHECK(xiaotai_json_get(&doc, stream, "down_video_streamid") < 0);
    }

    codec_array(&doc, call, "up_audio_mt", "alaw");
    codec_array(&doc, call, "down_audio_mt", "alaw");
    /* A device without H.264 sends JPEG to the other device and shows nothing itself. */
    codec_array(&doc, call, "up_video_mt", PRODUCT_TEST_PROFILE_EXPECT_CALL ? "mjpeg" : NULL);
    codec_array(&doc, call, "down_video_mt", NULL);
    boolean(&doc, call, "no_video", !PRODUCT_TEST_PROFILE_EXPECT_CALL);
    geometry(&doc, call, PRODUCT_TEST_PROFILE_EXPECT_CALL ? "15:11" : "4:3");     /* the picture it sends: 240 x 176 */

    integer(&doc, voip, "screen_width", 320);
    integer(&doc, voip, "screen_height", 240);
    integer(&doc, voip, "calling_timeout_sec", 30);
    integer(&doc, voip, "down_video_rotation", 0);
    string(&doc, voip, "down_audio_mt", "alaw");
    string(&doc, voip, "up_video_mt", "none");
    string(&doc, voip, "down_video_mt", "none");
    string(&doc, voip, "video_res_mode", "auto");
    boolean(&doc, voip, "no_video", true);
    geometry(&doc, voip, "4:3");
    atomic_store(&posted_bytes, request->body_size);
    CHECK(atomic_fetch_add(&posts, 1U) == 0U);
}

static void select_boundary_input(const char *probe)
{
    static char long_model[65];
    if (!probe || !*probe) return;
    override_model = true;
    reject_model = true;
    if (!strcmp(probe, "null")) expected_board = NULL;
    else if (!strcmp(probe, "empty")) expected_board = "";
    else if (!strcmp(probe, "quote")) expected_board = "quoted\"model";
    else if (!strcmp(probe, "backslash")) expected_board = "slash\\model";
    else if (!strcmp(probe, "newline")) expected_board = "line\nbreak";
    else if (!strcmp(probe, "space") || !strcmp(probe, "forward-slash")) {
        expected_board = !strcmp(probe, "space") ? "Fixture S3" : "Fixture/S3";
        reject_model = false;
    } else {
        CHECK(!strcmp(probe, "bytes63") || !strcmp(probe, "bytes64"));
        reject_model = !strcmp(probe, "bytes64");
        const size_t model_bytes = reject_model ? 64U : 63U;
        memset(long_model, 'A', model_bytes);
        long_model[model_bytes] = 0;
        expected_board = long_model;
    }
}

void product_profile_video_prepare(bool expect_h5, const char *board_model)
{
    expect_video = expect_h5;
    expected_board = board_model;
    CHECK(expected_board && *expected_board);
    select_boundary_input(getenv("XIAOTAI_HOST_PROFILE_MODEL_PROBE"));
    atomic_store(&posts, 0);
    atomic_store(&posted_bytes, 0);
    atomic_store(&document_capacity, 0);
    product_platform_prepare(true);
    product_platform_profile_observer(check_document);
}

void product_profile_video_run(void)
{
    product_platform_online();
    const int64_t until = xiaotai_time_us() + INT64_C(5000000);
    xiaotai_platform_status_t status;
    do {
        xiaotai_platform_status(&status);
        if (status.profile_ready) break;
        xiaotai_delay_ms(5);
    } while (xiaotai_time_us() < until);
    CHECK(status.profile_ready && atomic_load(&posts) == 1U);
    const size_t capacity = atomic_load(&document_capacity), bytes = atomic_load(&posted_bytes);
    CHECK(bytes && bytes < capacity);
    printf("%s: profile POST count=%u model_bytes=%zu wire_bytes=%zu slot=%zu H5=%u\n",
           XIAOTAI_PRODUCT_NAME, atomic_load(&posts), strlen(expected_board), bytes, capacity, (unsigned)expect_video);
}
