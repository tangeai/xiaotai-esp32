// SPDX-License-Identifier: MIT
#pragma once
/* Internal to the coprocessor part: the part object and the reader of the
 * application image format, shared by the boot check (coprocessor.c) and the
 * manual update and commands (coprocessor_update.c, work package A15). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "coprocessor_link.h"
#include "xiaotai/hal.h"
#include "xiaotai/parts/coprocessor.h"
#include "xiaotai/port/os.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Platform result retained for the version check, not yet named by the port. */
#ifndef XIAOTAI_ERR_INVALID_VERSION
#define XIAOTAI_ERR_INVALID_VERSION 0x10A    /* version not accepted */
#endif

/* Application image format of the coprocessor's boot loader (little endian):
 * a 24-byte image header, an 8-byte header of the first segment, then the
 * 256-byte application descriptor at the start of that segment. */
#define XIAOTAI_COPROCESSOR_IMAGE_MAGIC 0xE9U
#define XIAOTAI_COPROCESSOR_APP_MAGIC 0xABCD5432UL
#define XIAOTAI_COPROCESSOR_IMAGE_HEADER_BYTES 24U
#define XIAOTAI_COPROCESSOR_SEGMENT_HEADER_BYTES 8U
#define XIAOTAI_COPROCESSOR_APP_DESC_BYTES 256U
#define XIAOTAI_COPROCESSOR_IMAGE_MIN_BYTES \
    (XIAOTAI_COPROCESSOR_IMAGE_HEADER_BYTES + XIAOTAI_COPROCESSOR_SEGMENT_HEADER_BYTES + \
     XIAOTAI_COPROCESSOR_APP_DESC_BYTES)

/* Fields of an image the checks use. */
typedef struct {
    uint8_t magic;
    uint8_t segment_count;
    uint16_t chip_id;
    uint16_t min_revision;            /* major * 100 + minor */
    uint16_t max_revision;
    uint32_t first_segment_bytes;
    xiaotai_coprocessor_app_t app;    /* descriptor at the start of the first segment */
} xiaotai_coprocessor_image_info_t;

/* Reads the header fields; checks nothing but the length.
 * XIAOTAI_ERR_INVALID_SIZE: shorter than XIAOTAI_COPROCESSOR_IMAGE_MIN_BYTES. */
xiaotai_err_t xiaotai_coprocessor_image_read(const uint8_t *data, size_t size,
                                             xiaotai_coprocessor_image_info_t *info);

/* True when the text field holds a terminator within its width. */
bool xiaotai_coprocessor_text_terminated(const char text[XIAOTAI_COPROCESSOR_APP_TEXT]);

/* 64 hexadecimal digits (either case) to 32 bytes; false otherwise. */
bool xiaotai_coprocessor_hex_digest(const char *hex, uint8_t out[XIAOTAI_COPROCESSOR_DIGEST_BYTES]);

/* The part object: the network part handed to the board, and what prepare()
 * keeps between calls of one boot. Allocated once (internal RAM), never freed. */
typedef struct {
    xiaotai_network_t network;                    /* network.self points here */
    const xiaotai_coprocessor_config_t *config;
    xiaotai_critical_t lock;                      /* state and cache: read by [any] callers */
    xiaotai_coprocessor_state_t state;
    bool cached;                                  /* cache holds a complete identity */
    xiaotai_coprocessor_info_t cache;
    /* Worker-only (the engine serializes the hooks). */
    bool transfer_started;                        /* one transfer per boot */
    uint8_t transfer_rule;                        /* 8 or 9: the rule that started it */
} xiaotai_coprocessor_part_t;

xiaotai_err_t xiaotai_coprocessor_manual_update(void *self, const uint8_t *data, size_t size,
                                               xiaotai_network_progress_fn_t progress, void *ctx);
xiaotai_err_t xiaotai_coprocessor_command(void *self, const char *command, char *reply, size_t capacity);

#ifdef __cplusplus
}
#endif
