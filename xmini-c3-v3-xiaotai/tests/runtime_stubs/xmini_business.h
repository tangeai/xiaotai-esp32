// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef struct {
    bool (*receive)(uint32_t generation, uint8_t stream, uint32_t pts, const uint8_t *packet, size_t length, void *context);
    int (*media_start)(uint32_t generation, uint8_t stream, void *context);
    int (*media_stop)(uint32_t generation, void *context);
    bool (*media_busy)(uint32_t generation, void *context);
    void (*media_flush)(uint32_t generation, void *context);
    void *context;
} xmini_business_callbacks_t;
int xmini_business_init(const xmini_business_callbacks_t *callbacks);
void xmini_business_process(void);
int xmini_business_maintenance(bool active);
int xmini_business_send_audio(uint32_t generation, const uint8_t *packet, size_t length, uint32_t pts);
