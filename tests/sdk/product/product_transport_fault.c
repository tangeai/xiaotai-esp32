// SPDX-License-Identifier: MIT
/* A fault of the RTC transport is not the end of the boot: the colour products
 * leave it by themselves (parameters.transport.fault_restart_ms). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "product_platform.h"
#include "product_test.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/os.h"
#include "xiaotai/transport.h"
#include "xiaotai_transport_host.h"

#define RESTART_MS 5000U
#define WAIT_MS 4000U

void product_transport_fault_prepare(void)
{
    product_platform_prepare(true);
    xiaotai_transport_host_clock_manual(10000);
}

static xiaotai_transport_status_t settled(void)
{
    assert(xiaotai_transport_host_settle(WAIT_MS));
    product_on_owner(NULL, NULL);
    xiaotai_transport_status_t status;
    xiaotai_transport_status(&status);
    return status;
}

static uint32_t starts(void) { return xiaotai_transport_host_count(XIAOTAI_TRANSPORT_HOST_START); }

/* Online again, with signaling back; what the start waits for runs in real time. */
static void await_online(void)
{
    xiaotai_transport_status_t rtc;
    xiaotai_platform_status_t platform;
    for (unsigned waited = 0; waited < WAIT_MS; waited += 5U) {
        rtc = settled();
        xiaotai_platform_status(&platform);
        if (rtc.state == XIAOTAI_TRANSPORT_ONLINE && rtc.connect_ready &&
            platform.phase == XIAOTAI_PLATFORM_ONLINE && platform.signaling) return;
        xiaotai_delay_ms(5);
    }
    fprintf(stderr, "%s: RTC state=%u wait=%u connect_ready=%u, platform phase=%u signaling=%u\n",
            XIAOTAI_PRODUCT_NAME, (unsigned)rtc.state, (unsigned)rtc.wait, (unsigned)rtc.connect_ready,
            (unsigned)platform.phase, (unsigned)platform.signaling);
    abort();
}

void product_transport_fault(void)
{
    product_platform_online();
    await_online();
    const uint32_t before = starts();
    /* Again and again, not only once. */
    for (uint32_t round = 1; round <= 3U; ++round) {
        xiaotai_transport_host_stopped(0); /* Nobody asked for it. */
        xiaotai_transport_status_t rtc = settled();
        assert(rtc.state == XIAOTAI_TRANSPORT_FAULT && rtc.wait == XIAOTAI_TRANSPORT_WAIT_FAULT);
        assert(rtc.faults == round && rtc.unsolicited_stops == round);
        xiaotai_transport_host_clock_advance(RESTART_MS - 1U);
        rtc = settled();
        assert(rtc.state == XIAOTAI_TRANSPORT_FAULT && starts() == before + round - 1U);
        xiaotai_transport_host_clock_advance(1U);
        await_online();
        assert(starts() == before + round);
    }
    printf("%s: three faults, online again after %u ms each\n", XIAOTAI_PRODUCT_NAME, RESTART_MS);
}
