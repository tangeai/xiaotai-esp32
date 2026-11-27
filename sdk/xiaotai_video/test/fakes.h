// SPDX-License-Identifier: MIT
#pragma once
/* Shared fakes of the video engine's host tests:
 *   - the hardware abstraction's part queries (xiaotai_hal_camera,
 *     xiaotai_hal_display) answered from tables the test fills, with fake
 *     camera parts that record their calls in order;
 *   - encoder, decoder and transform backends, FAKE_BACKENDS of each kind,
 *     each with its own match() whose score and filters the test sets;
 *   - a link that records what the engine hands it;
 *   - recorders of port calls, linked with --wrap (test/CMakeLists.txt):
 *     allocations by memory kind, task creations with their configuration,
 *     and a clock the test can hold still and move.
 * The engine's sources are compiled into each test; the hardware abstraction
 * is replaced (INCLUDE_ONLY), the port, framework and event log are real. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "xiaotai/hal.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/video.h"
#include "xiaotai/video_backend.h"

#define CHECK(cond, ...)                                                                \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            fprintf(stderr, "%s:%d: CHECK(%s) failed:", __FILE__, __LINE__, #cond);     \
            fprintf(stderr, " " __VA_ARGS__);                                           \
            fprintf(stderr, "\n");                                                      \
            exit(1);                                                                    \
        }                                                                               \
    } while (0)

/* ------------------------------------------------------------ hardware */

#define FAKE_CAMERAS_MAX 4U
#define FAKE_DISPLAYS_MAX 8U
#define FAKE_CAMERA_LOG_MAX 64U

typedef enum {
    FAKE_CAMERA_PREPARE = 1,
    FAKE_CAMERA_POWER_ON,
    FAKE_CAMERA_POWER_OFF,
    FAKE_CAMERA_OPEN,
    FAKE_CAMERA_CLOSE,
    FAKE_CAMERA_BORROW,
    FAKE_CAMERA_RELEASE,
} fake_camera_call_t;

typedef struct {
    xiaotai_camera_t part;
    /* Results the calls return; XIAOTAI_OK unless the test sets another. */
    xiaotai_err_t prepare_result, power_result, open_result, close_result, borrow_result;
    fake_camera_call_t log[FAKE_CAMERA_LOG_MAX];
    size_t log_count;
} fake_camera_t;

/* Forget every part. */
void fake_hal_reset(void);
/* Adds camera part number <count>; with_prepare gives it a prepare(). */
fake_camera_t *fake_camera_add(const xiaotai_camera_desc_t *desc, bool with_prepare);
/* Adds display part number <count> of that logical size, pixel and flags. */
xiaotai_display_t *fake_display_add(uint16_t width, uint16_t height, xiaotai_pixel_t pixel, uint32_t flags);

/* ------------------------------------------------------------ backends */

#define FAKE_BACKENDS 12U

/* Control block of one fake backend. match() returns score when every filter
 * passes (NONE / 0: any), otherwise 0. */
typedef struct {
    int score;
    xiaotai_video_codec_t codec;    /* encoders, decoders */
    xiaotai_video_pixel_t pixel;    /* encoders: input layout; transforms: output layout */
    xiaotai_video_pixel_t from;     /* transforms: input layout limits() accepts */
    uint16_t max_width;             /* encoders: widest picture accepted; 0 any */
    xiaotai_err_t open_result;
    unsigned match_calls, open_calls, close_calls;
    xiaotai_video_encoder_config_t last_encoder;
    xiaotai_video_encoder_config_t matched_encoder;
    xiaotai_video_decoder_config_t last_decoder;
    void *state;                    /* what open() hands out */
} fake_backend_t;

extern fake_backend_t fake_encoder_ctl[FAKE_BACKENDS];
extern fake_backend_t fake_decoder_ctl[FAKE_BACKENDS];
extern fake_backend_t fake_transform_ctl[FAKE_BACKENDS];
/* Descriptors; tests may change name, load, flags and stack fields before
 * registering them. */
extern xiaotai_video_encoder_t fake_encoders[FAKE_BACKENDS];
extern xiaotai_video_decoder_t fake_decoders[FAKE_BACKENDS];
extern xiaotai_video_transform_t fake_transforms[FAKE_BACKENDS];

/* Restores every descriptor and control block to its initial values: score
 * 0, no filter, load SOFTWARE, no flags, stack 0. */
void fake_backends_reset(void);

/* ---------------------------------------------------------------- link */

typedef struct {
    bool ready;
    size_t backlog;
    xiaotai_err_t send_result;
    unsigned ready_calls, send_calls, key_requests;
    uint32_t last_generation;
    xiaotai_video_packet_t last_packet;   /* data is not kept */
} fake_link_state_t;

extern fake_link_state_t fake_link_state;
/* A link with every member, ctx = &fake_link_state. */
const xiaotai_video_link_t *fake_link(void);

/* ------------------------------------------------------- port recorders */

typedef struct {
    char name[32];
    xiaotai_task_config_t config;
} fake_task_record_t;

#define FAKE_TASK_RECORDS 16U

/* Allocations through the memory port by kind since the last clear (failed
 * ones included), and task creations. */
void fake_port_clear(void);
unsigned fake_mem_allocations(xiaotai_mem_kind_t kind);
unsigned fake_task_count(void);
const fake_task_record_t *fake_task_at(size_t index);
const fake_task_record_t *fake_task_named(const char *name);
/* The n-th task creation from now (1: the next) fails with error, once. */
void fake_task_fail_nth(unsigned n, xiaotai_err_t error);

/* Clock: while held, xiaotai_time_us() returns the held value. */
void fake_clock_hold(int64_t us);
void fake_clock_advance_us(int64_t us);
void fake_clock_release(void);
