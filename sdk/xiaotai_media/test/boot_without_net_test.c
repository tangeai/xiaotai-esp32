// SPDX-License-Identifier: MIT
#include <assert.h>
#include "internal.h"

int main(void)
{
    const xiaotai_media_requirement_t need = {
        .downlink = XIAOTAI_MEDIA_FORMAT_G711A_8K, .downlink_burst_ms = 1000,
    };
    assert(!media_provisioning_boot());
    assert(xiaotai_media_require(&need) == XIAOTAI_OK);
    xiaotai_engine_media_start();
    assert(xiaotai_media_can_receive(&need.downlink));
    assert(xiaotai_downlink_capacity(xiaotai_media_downlink()) == XIAOTAI_DOWNLINK_BYTES(60U, 160U));
    xiaotai_media_reset();
    return 0;
}
