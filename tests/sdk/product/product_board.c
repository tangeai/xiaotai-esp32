// SPDX-License-Identifier: MIT
#include "product_test.h"
#include "xiaotai_bus_host.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#if PRODUCT_TEST_COPROCESSOR
#include "camera_csi_fake.h"
#include "xiaotai_coprocessor_host.h"

static void seed_c6(void)
{
    /* ELF identity of the board package's checked 3.0.7-xiaotai.5 image.
     * Boot exercises the real read-and-match path; no simulated update. */
    static const uint8_t elf_sha256[] = {
        0x0c, 0x5f, 0x27, 0xf9, 0xc6, 0x5b, 0x06, 0x35, 0xb1, 0x96, 0xbb, 0x68, 0x47, 0x02, 0x88, 0x21,
        0x0f, 0xa6, 0xab, 0x4f, 0x90, 0xbc, 0xe7, 0xc8, 0x7e, 0xa5, 0xc3, 0xe5, 0xf2, 0xd5, 0xf5, 0xd5,
    };
    xiaotai_coprocessor_host_t *link = xiaotai_coprocessor_host();
    link->chip = 0x0d;
    link->major = 3;
    link->patch = 7;
    link->app_magic = UINT32_C(0xabcd5432);
    memcpy(link->project, "eh_cp_wifi_sta", sizeof("eh_cp_wifi_sta"));
    memcpy(link->version, "3.0.7-xiaotai.5", sizeof("3.0.7-xiaotai.5"));
    memcpy(link->platform, "v5.5.4", sizeof("v5.5.4"));
    memcpy(link->elf_sha256, elf_sha256, sizeof(elf_sha256));
    csi_fake.powered = true;   /* Fixed rail, as in the Waveshare board fixture. */
}

static void seed_c61(void)
{
    xiaotai_coprocessor_host_t *link = xiaotai_coprocessor_host();
    link->chip = 0x14;
    link->major = 3;
    link->patch = 7;
    csi_fake.powered = true;
    csi_fake.reg[0x3107] = 0xcb;
    csi_fake.reg[0x3108] = 0x3a;
}
#endif

typedef struct {
    const char *board;
    int port;
    uint8_t devices[3];
    void (*prepare)(void);
} wiring_t;

/* Only what the host-side drivers probe or use. The real board factory still
 * supplies every descriptor, creates the parts and checks its own wiring. */
static const wiring_t boards[] = {
    {"lckfb/szpi-esp32s3", 0, {0x19, 0x38}, NULL},
    {"m5stack/core-s3", 0, {0x58, 0x34, 0x38}, NULL},
    {"xmini/c3-v3", 0, {0x18, 0x3c}, NULL},
    {"guition/jc1060f570c", 0, {0x5d, 0x10}, NULL},
#if PRODUCT_TEST_COPROCESSOR
    {"waveshare/esp32-p4-wifi6-touch-lcd-3.5", 1, {0x38}, seed_c6},
    {"wt/wt99p461-s1", 1, {0x5d}, seed_c61},
    {"wt/wt9932p4c61-tiny", 0, {0}, seed_c61},     /* the camera alone on I2C0, as SCCB */
#endif
};

void product_board_prepare(void)
{
    for (size_t i = 0; i < sizeof(boards) / sizeof(boards[0]); ++i) {
        const wiring_t *wiring = &boards[i];
        if (strcmp(wiring->board, XIAOTAI_PRODUCT_BOARD)) continue;
        for (size_t dev = 0; dev < sizeof(wiring->devices) && wiring->devices[dev]; ++dev) {
            assert(xiaotai_bus_host_i2c_attach(wiring->port, wiring->devices[dev]) == XIAOTAI_OK);
        }
        if (wiring->prepare) wiring->prepare();
        return;
    }
    fprintf(stderr, "No host wiring fixture for the selected board %s\n", XIAOTAI_PRODUCT_BOARD);
    assert(false);
}
