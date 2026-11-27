// SPDX-License-Identifier: MIT
#pragma once
/* The standard carrier of a video session: the transport link (xiaotai/link.h)
 * whose generation is the session's generation.
 *
 * A module that runs video over a transport link writes no glue:
 *
 *   xiaotai_link_receive(generation, XIAOTAI_LINK_VIDEO, xiaotai_video_transport_receive, NULL);
 *   session.link = xiaotai_video_transport_link();
 *   xiaotai_video_open(&session);
 *
 * Stream numbers, receive formats and whether the peer must subscribe are the
 * link's profile, given by the module when it requested the link; nothing here
 * knows them. This is the only part of the engine that includes a transport
 * header: its pipelines talk to xiaotai_video_link_t alone, so another carrier
 * is one more object of that type. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/link.h"
#include "xiaotai/video.h"

#ifdef __cplusplus
extern "C" {
#endif

/* [any] The link object for xiaotai_video_session_t.link; static, valid
 * forever, ctx unused. Its functions map one to one:
 *   ready                   xiaotai_link_can_send(generation, XIAOTAI_LINK_VIDEO)
 *   backlog                 xiaotai_link_backlog(generation)
 *   send                    xiaotai_link_send_video(): codec, key frame flag,
 *                           timestamp and backlog limit passed through; the
 *                           transport's results are the link's results
 *   take_key_frame_request  xiaotai_link_take_key_frame_request(generation)
 *   request_key_frame       xiaotai_link_request_key_frame(generation)
 *   set_bitrate_range       xiaotai_link_set_video_bitrate(generation, min, start, max)
 *   take_bitrate_target     xiaotai_link_take_video_bitrate(generation, &bps)
 * xiaotai_link_send_video() and xiaotai_link_backlog() are [uplink] in
 * xiaotai/link.h; the engine calls send and backlog only on its camera task,
 * which is such a task, so this mapping needs no task of its own. */
const xiaotai_video_link_t *xiaotai_video_transport_link(void);

/* [receive thread] A xiaotai_link_receive_fn_t that feeds the session of the
 * same generation through xiaotai_video_push(): one bounded copy, no blocking,
 * allocation or logging. Frames of a media kind other than video, or in a codec
 * the engine has no name for, are refused (false). ctx is unused. */
bool xiaotai_video_transport_receive(void *ctx, uint32_t generation, const xiaotai_link_frame_t *frame,
                                     const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif
