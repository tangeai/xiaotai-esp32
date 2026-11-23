// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_p4_h264_feedback.py
#include "h264_au.h"
#include <assert.h>

static const uint8_t sps[] = {0, 0, 0, 1, 0x67, 66, 0x40, 31, 0x80};
static const uint8_t sps2[] = {0, 0, 1, 0x67, 66, 0x40, 31, 0x44, 0x80};
static const uint8_t pps[] = {0, 0, 1, 0x68, 0x80};
static const uint8_t pps2[] = {0, 0, 1, 0x68, 0x40};
static const uint8_t idr[] = {0, 0, 1, 0x65, 0x80};
static const uint8_t picture[] = {0, 0, 1, 0x61, 0x80};
static const uint8_t high[] = {0, 0, 0, 1, 0x67, 100, 0, 31, 0x80};

static video_h264_admission_t admit(video_h264_chain_t *chain, const uint8_t *data, size_t bytes,
                                    bool link_key, bool baseline, bool recover)
{
    const video_h264_au_t au = video_h264_inspect(data, bytes);
    return video_h264_admit(chain, &au, link_key, baseline, recover, false);
}

#define ADMIT(chain, arr) admit(chain, arr, sizeof(arr), false, true, false)

int main(void)
{
    const uint8_t full[] = {0, 0, 0, 1, 0x67, 66, 0x40, 31, 0x80,
                            0, 0, 1, 0x68, 0x80, 0, 0, 1, 0x65, 0x80, 0, 0};
    const uint8_t no_nal[] = {0, 0, 0, 1};
    const uint8_t truncated[] = {0, 0, 1, 0x67, 66, 0x40};
    const uint8_t avcc[] = {0, 0, 0, 2, 0x65, 0x80};
    const uint8_t junk[] = {0xcc, 0, 0, 1, 0x65, 0x80};
    const uint8_t forbidden[] = {0, 0, 1, 0xe5, 0x80};
    const uint8_t mixed[] = {0, 0, 1, 0x67, 100, 0, 31, 0x80, 0, 0, 1, 0x67, 66, 0x40, 31, 0x80};
    video_h264_au_t info = video_h264_inspect(full, sizeof(full));
    assert(info.valid && info.sps && info.pps && info.idr && info.constrained_baseline);
    assert(info.profile == 66 && info.constraints == 0x40 && info.level == 31);
    assert(info.sps_hash == video_h264_inspect(sps, sizeof(sps)).sps_hash);
    assert(info.pps_hash == video_h264_inspect(pps, sizeof(pps)).pps_hash);
    assert(!video_h264_inspect(NULL, 100).valid && !video_h264_inspect(no_nal, sizeof(no_nal)).valid);
    assert(!video_h264_inspect(truncated, sizeof(truncated)).valid && !video_h264_inspect(avcc, sizeof(avcc)).annex_b);
    assert(!video_h264_inspect(junk, sizeof(junk)).valid && !video_h264_inspect(forbidden, sizeof(forbidden)).valid);
    assert(!video_h264_inspect(mixed, sizeof(mixed)).constrained_baseline);

    video_h264_chain_t chain;
    video_h264_chain_reset(&chain);
    assert(ADMIT(&chain, picture).status == VIDEO_H264_WAIT_KEY);
    assert(ADMIT(&chain, pps).status == VIDEO_H264_WAIT_KEY);
    assert(ADMIT(&chain, idr).status == VIDEO_H264_WAIT_KEY);
    assert(ADMIT(&chain, truncated).status == VIDEO_H264_BAD_FORMAT && chain.waiting_key);
    assert(ADMIT(&chain, sps).status == VIDEO_H264_ADMIT && chain.waiting_key && chain.sps_ready);
    assert(ADMIT(&chain, pps).status == VIDEO_H264_ADMIT && chain.waiting_key && chain.pps_ready);
    assert(ADMIT(&chain, picture).status == VIDEO_H264_WAIT_KEY);
    assert(ADMIT(&chain, idr).key && !chain.waiting_key);
    assert(ADMIT(&chain, picture).status == VIDEO_H264_ADMIT);
    assert(!ADMIT(&chain, sps).reference_changed && !chain.waiting_key);
    assert(ADMIT(&chain, pps2).reference_changed && chain.waiting_key && !chain.sps_ready);
    assert(ADMIT(&chain, idr).status == VIDEO_H264_WAIT_KEY);
    assert(ADMIT(&chain, full).status == VIDEO_H264_ADMIT && !chain.waiting_key);
    assert(ADMIT(&chain, sps2).reference_changed && chain.waiting_key && !chain.pps_ready);
    assert(ADMIT(&chain, pps).status == VIDEO_H264_ADMIT);
    assert(admit(&chain, picture, sizeof(picture), true, true, false).key && !chain.waiting_key);

    assert(ADMIT(&chain, high).status == VIDEO_H264_UNSUPPORTED && chain.unsupported);
    assert(ADMIT(&chain, full).status == VIDEO_H264_UNSUPPORTED && chain.unsupported);
    assert(admit(&chain, pps, sizeof(pps), false, true, true).status == VIDEO_H264_UNSUPPORTED);
    assert(admit(&chain, sps, sizeof(sps), false, true, true).recovered && chain.waiting_key && !chain.unsupported);
    assert(ADMIT(&chain, idr).status == VIDEO_H264_WAIT_KEY);
    assert(ADMIT(&chain, full).status == VIDEO_H264_ADMIT && !chain.waiting_key);
    video_h264_chain_reset(&chain);
    assert(admit(&chain, high, sizeof(high), false, false, false).status == VIDEO_H264_ADMIT && !chain.unsupported);
    video_h264_chain_reset(&chain);
    assert(ADMIT(&chain, full).status == VIDEO_H264_ADMIT);
    info = video_h264_inspect(pps2, sizeof(pps2));
    assert(!video_h264_admit(&chain, &info, false, true, true, true).reference_changed && !chain.waiting_key);
    assert(chain.sps_ready && chain.pps_ready && ADMIT(&chain, idr).status == VIDEO_H264_ADMIT);
    info = video_h264_inspect(sps2, sizeof(sps2));
    assert(!video_h264_admit(&chain, &info, false, true, true, true).reference_changed && !chain.waiting_key);
    assert(chain.sps_ready && !chain.pps_ready && ADMIT(&chain, idr).status == VIDEO_H264_WAIT_KEY);
    return 0;
}
