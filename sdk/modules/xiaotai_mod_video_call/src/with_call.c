// SPDX-License-Identifier: MIT
#include "video_call_internal.h"
#include "xiaotai/modules/call.h"

static bool match(uint32_t link)
{
    xiaotai_call_status_t status;
    xiaotai_call_status(&status);
    return status.link == link && status.state == XIAOTAI_CALL_ACTIVE && (status.flags & XIAOTAI_CALL_VIDEO);
}

static xiaotai_err_t dial_contact(size_t index)
{
    return xiaotai_call_dial_contact(index, XIAOTAI_CALL_VIDEO, NULL);
}

const video_call_host_t video_call_with_call = {
    .scene = XIAOTAI_CALL_SCENE,
    .match = match,
    .dial_contact = dial_contact,
    .peer_codec = XIAOTAI_VIDEO_CODEC_H264,
    .profile_as_arrays = true,
};
