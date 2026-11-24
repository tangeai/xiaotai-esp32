// SPDX-License-Identifier: MIT
#include "clip_playout.h"
#include "xiaotai/framework.h"

xiaotai_err_t xiaotai_media_play_stream(const xiaotai_media_stream_t *request, uint32_t *id)
{
    return media_session_play_stream(request, id);
}

xiaotai_err_t xiaotai_media_play_write(uint32_t id, const void *data, size_t size, uint32_t timeout_ms)
{
    if (!data || !size) return XIAOTAI_ERR_INVALID_ARG;
    if (xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    media_stream_buffer_t *stream = media_session_find_stream(id);
    if (!stream) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_err_t err = media_stream_buffer_write(stream, data, size, timeout_ms);
    media_stream_buffer_release(stream);
    if (err == XIAOTAI_OK) media_session_wake_playout();
    return err;
}

xiaotai_err_t xiaotai_media_play_finish(uint32_t id)
{
    media_stream_buffer_t *stream = media_session_find_stream(id);
    if (!stream) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_err_t err = media_stream_buffer_finish(stream);
    media_stream_buffer_release(stream);
    if (err == XIAOTAI_OK) media_session_wake_playout();
    return err;
}
