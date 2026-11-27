// SPDX-License-Identifier: MIT
#include <assert.h>
#include "xiaotai/media.h"
#include "xiaotai_mem_host.h"
#include "internal.h"

static bool provisioning;
static unsigned boot_reads;

bool __wrap_xiaotai_net_provisioning_boot(void);
bool __wrap_xiaotai_net_provisioning_boot(void)
{
    ++boot_reads;
    return provisioning;
}

int main(void)
{
    const xiaotai_media_requirement_t ai = {
        .packet_ms = 20, .downlink_burst_ms = 1000, .preroll_ms = 20000,
        .uplink = XIAOTAI_MEDIA_FORMAT_G711A_8K, .downlink = XIAOTAI_MEDIA_FORMAT_G711A_8K,
    };
    const xiaotai_media_session_t playback = {
        .generation = 1, .downlink = XIAOTAI_MEDIA_FORMAT_G711A_8K, .downlink_burst_ms = 1000,
    };
    xiaotai_mem_host_clear();
    provisioning = true;
    assert(xiaotai_engine_media_init() == XIAOTAI_OK);
    assert(xiaotai_media_require(&ai) == XIAOTAI_OK);
    xiaotai_engine_media_start();
    provisioning = false;
    xiaotai_engine_media_start();
    assert(boot_reads == 1 && xiaotai_mem_host_allocations() == 0);
    xiaotai_media_info_t info;
    xiaotai_media_info(&info);
    assert(info.playback && !info.downlink_ms && !info.preroll_ms);
    assert(!xiaotai_media_downlink());
    assert(!xiaotai_media_can_send(&ai.uplink) && !xiaotai_media_can_receive(&ai.downlink));
    assert(media_session_validate(&playback) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_media_reset();

    assert(xiaotai_engine_media_init() == XIAOTAI_OK);
    assert(xiaotai_media_require(&ai) == XIAOTAI_OK);
    xiaotai_media_requirement_t alternate = ai;
    alternate.downlink.packet_ms = 40;
    alternate.downlink_burst_ms = 200;
    assert(xiaotai_media_require(&alternate) == XIAOTAI_OK);
    xiaotai_engine_media_start();
    xiaotai_media_info(&info);
    assert(boot_reads == 2 && xiaotai_mem_host_outstanding() > 0);
    assert(info.playback && info.downlink_ms == 1200);
    assert(xiaotai_media_can_receive(&ai.downlink));
    assert(xiaotai_downlink_capacity(xiaotai_media_downlink()) == XIAOTAI_DOWNLINK_BYTES(60U, 160U));
    assert(media_session_validate(&playback) == XIAOTAI_OK);
    xiaotai_media_session_t invalid = playback;
    invalid.downlink = alternate.downlink;
    assert(media_session_validate(&invalid) == XIAOTAI_ERR_INVALID_SIZE);
    invalid.downlink_burst_ms = 200;
    assert(media_session_validate(&invalid) == XIAOTAI_OK);
    invalid.downlink.packet_ms = 10;
    assert(media_session_validate(&invalid) == XIAOTAI_ERR_NOT_SUPPORTED);
    invalid.downlink.packet_ms = 0;
    assert(media_session_validate(&invalid) == XIAOTAI_ERR_INVALID_ARG);
    xiaotai_media_reset();
    assert(xiaotai_mem_host_outstanding() == 0);
    return 0;
}
