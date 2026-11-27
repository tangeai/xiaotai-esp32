// SPDX-License-Identifier: MIT
/* The host half of the video engine: the host has no codec library and no
 * accelerator, so it registers no backend. The engine's own software
 * transform is portable and registered by the engine itself; host tests
 * register fake encoders, decoders and transforms of their own. */
#include "xiaotai/video_backend.h"

xiaotai_err_t xiaotai_video_port_init(void) { return XIAOTAI_OK; }
