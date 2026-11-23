// SPDX-License-Identifier: MIT
#include "h264_au.h"

enum { NAL_IDR = 5, NAL_SPS = 7, NAL_PPS = 8, BASELINE = 66, CONSTRAINT_SET1 = 0x40 };

typedef struct { size_t zero, nal; } prefix_t;

static bool find_prefix(const uint8_t *data, size_t size, size_t from, prefix_t *prefix)
{
    size_t zeros = 0;
    for (size_t i = from; i < size; ++i) {
        if (data[i] == 0) ++zeros;
        else {
            if (data[i] == 1 && zeros >= 2) {
                *prefix = (prefix_t){.zero = i - zeros, .nal = i + 1};
                return true;
            }
            zeros = 0;
        }
    }
    return false;
}

static uint64_t hash_nal(const uint8_t *data, size_t size)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * UINT64_C(1099511628211);
    return hash;
}

static uint64_t combine_nal_hash(uint64_t prior, uint64_t next, bool has_prior)
{
    return has_prior ? ((prior << 7) | (prior >> 57)) ^ next : next;
}

video_h264_au_t video_h264_inspect(const uint8_t *data, size_t size)
{
    video_h264_au_t au = {0};
    prefix_t current;
    if (!data || !find_prefix(data, size, 0, &current) || current.zero != 0) return au;
    au.annex_b = true;
    au.constrained_baseline = true;
    while (current.nal < size) {
        prefix_t next;
        const bool more = find_prefix(data, size, current.nal, &next);
        size_t end = more ? next.zero : size;
        while (end > current.nal && data[end - 1] == 0) --end;  /* Annex-B padding is not RBSP. */
        if (end <= current.nal || (data[current.nal] & 0x80U)) return (video_h264_au_t){.annex_b = true};
        const uint8_t *nal = data + current.nal;
        const size_t bytes = end - current.nal;
        au.valid = true;
        switch (nal[0] & 31U) {
        case NAL_SPS:
            if (bytes < 4) return (video_h264_au_t){.annex_b = true, .sps = true};
            au.sps_hash = combine_nal_hash(au.sps_hash, hash_nal(nal, bytes), au.sps);
            au.sps = true;
            au.profile = nal[1];
            au.constraints = nal[2];
            au.level = nal[3];
            au.constrained_baseline &= au.profile == BASELINE && (au.constraints & CONSTRAINT_SET1);
            break;
        case NAL_PPS:
            au.pps_hash = combine_nal_hash(au.pps_hash, hash_nal(nal, bytes), au.pps);
            au.pps = true;
            break;
        case NAL_IDR:
            if (!au.idr) au.idr_with_headers = au.sps && au.pps;
            au.idr = true;
            break;
        default:
            break;
        }
        if (!more) break;
        current = next;
    }
    return au;
}

void video_h264_chain_reset(video_h264_chain_t *chain)
{
    if (chain) *chain = (video_h264_chain_t){.waiting_key = true};
}

video_h264_admission_t video_h264_admit(video_h264_chain_t *chain, const video_h264_au_t *au,
                                        bool link_key, bool baseline_only, bool recover_profile,
                                        bool preserve_parameter_chain)
{
    if (!chain || !au || !au->valid) return (video_h264_admission_t){.status = VIDEO_H264_BAD_FORMAT};
    const bool incompatible = au->sps && baseline_only && !au->constrained_baseline;
    if (incompatible) {
        video_h264_chain_reset(chain);
        chain->unsupported = true;
        return (video_h264_admission_t){.status = VIDEO_H264_UNSUPPORTED};
    }
    const bool recovered = chain->unsupported && recover_profile && au->sps;
    if (chain->unsupported && !recovered) return (video_h264_admission_t){.status = VIDEO_H264_UNSUPPORTED};

    const bool sps_changed = au->sps && (!chain->sps_ready || chain->sps_hash != au->sps_hash);
    const bool pps_changed = au->pps && (!chain->pps_ready || chain->pps_hash != au->pps_hash);
    const bool changed = (chain->sps_ready && sps_changed) || (chain->pps_ready && pps_changed);
    if (recovered || (changed && !preserve_parameter_chain)) video_h264_chain_reset(chain);
    else if (preserve_parameter_chain) {
        if (sps_changed) chain->sps_ready = false;
        if (sps_changed || pps_changed) chain->pps_ready = false;
    }
    video_h264_admission_t result = {.key = link_key || au->idr,
        .reference_changed = changed && !preserve_parameter_chain, .recovered = recovered};
    const bool sps = chain->sps_ready || au->sps;
    const bool pps = chain->pps_ready || au->pps;
    if ((result.key && (!sps || !pps)) ||
        (chain->waiting_key && (!result.key && !au->sps && (!au->pps || !sps)))) {
        result.status = VIDEO_H264_WAIT_KEY;
        return result;
    }
    if (au->sps) { chain->sps_ready = true; chain->sps_hash = au->sps_hash; }
    if (au->pps) { chain->pps_ready = true; chain->pps_hash = au->pps_hash; }
    if (result.key) chain->waiting_key = false;
    result.status = VIDEO_H264_ADMIT;
    return result;
}
