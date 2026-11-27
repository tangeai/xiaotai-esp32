// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/video.h"

/* Presentation owner supplies a descriptor for pixels it already pinned.
 * No buffering or borrowing happens here; clear before the frame's slot is
 * returned to the producer. Called by presenter, or owner after presenter stopped. */
void video_paint_pin(const xiaotai_video_frame_t *frame);
