// SPDX-License-Identifier: MIT
#include "harness.h"
#include "xiaotai/modules/ai_talk.h"

int main(void)
{
    fakes_reset();
    CHECK(harness_start_without_vision() == 0);
    CHECK(owner_contributions(XIAOTAI_AI_TALK_ACTIVITY, XIAOTAI_LINK_SLOT_VIDEO,
                              XIAOTAI_AI_TALK_SCENE, NULL, 0) == 0);
    const uint32_t id = owner_ai_begin_at(10);
    CHECK(id != 0 && fake_video.opens == 0 && fake_transport.permits == 0);
    owner_ai_end(id);
    harness_stop();

    CHECK(harness_start(NULL, true) == 0);
    const void *out[2] = {0};
    /* The descriptor is available before the session is accepted. */
    CHECK(owner_contributions(XIAOTAI_AI_TALK_ACTIVITY, XIAOTAI_LINK_SLOT_VIDEO,
                              XIAOTAI_AI_TALK_SCENE, out, 2) == 1);
    const xiaotai_link_media_t *part = out[0];
    CHECK(part && out[1] == NULL);
    CHECK(part->send && part->send_stream == 0);
    CHECK(part->peer_streams == XIAOTAI_LINK_PEER_STREAM_REFUSE);
    CHECK(part->send_format.codec == XIAOTAI_LINK_CODEC_JPEG);
    CHECK(part->send_format.rate_hz == 0 && part->send_format.channels == 0);
    CHECK(!part->receive && !part->receive_request && part->receive_stream == 0);
    for (size_t i = 0; i < XIAOTAI_LINK_RECEIVE_FORMATS; ++i) {
        CHECK(part->receive_formats[i].codec == XIAOTAI_LINK_CODEC_NONE);
    }
    CHECK(owner_contributions("call", XIAOTAI_LINK_SLOT_VIDEO, XIAOTAI_AI_TALK_SCENE,
                              NULL, 0) == 0);
    CHECK(owner_contributions(XIAOTAI_AI_TALK_ACTIVITY, XIAOTAI_LINK_SLOT_VIDEO,
                              "call", NULL, 0) == 0);
    CHECK(fake_video.opens == 0 && fake_transport.permits == 0);
    harness_stop();
    return 0;
}
