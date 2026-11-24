// SPDX-License-Identifier: MIT
#include "video_call_internal.h"
#include "xiaotai/modules/wechat.h"

static bool match(uint32_t link)
{
    xiaotai_wechat_status_t status;
    xiaotai_wechat_status(&status);
    return status.link == link && status.state == XIAOTAI_WECHAT_ACTIVE && (status.flags & XIAOTAI_WECHAT_VIDEO);
}

static xiaotai_err_t dial_contact(size_t index)
{
    return xiaotai_wechat_dial_contact(index, XIAOTAI_WECHAT_VIDEO, NULL);
}

const video_call_host_t video_call_with_wechat = {
    .scene = XIAOTAI_WECHAT_SCENE,
    .match = match,
    .dial_contact = dial_contact,
    .peer_codec = XIAOTAI_VIDEO_CODEC_JPEG,
    .subscribed = xiaotai_wechat_request_video,
};
