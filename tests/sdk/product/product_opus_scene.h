// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>

void product_opus_legacy_scene(uintptr_t peer, uint32_t generation, const char *scene,
                               uint8_t receive_stream, bool accepts_peer_stream);
