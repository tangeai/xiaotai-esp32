// SPDX-License-Identifier: MIT
#pragma once
/* Parts library: a network part of kind XIAOTAI_NETWORK_WIFI_COPROCESSOR - a
 * coprocessor that runs the Wi-Fi stack and is reached over a host link
 * (SDIO or SPI; the bus and its pins are platform configuration of the board).
 *
 * The part implements the hooks of xiaotai_network_t: prepare() brings the
 * link up, reads the coprocessor's identity, decides whether its firmware is
 * usable and, only when the board carries an image and the rules below allow
 * it, updates it; coprocessor_info(), coprocessor_update() and
 * coprocessor_command() serve diagnostics and manual maintenance.
 *
 * Decision rules of prepare(), in order (each board enables the ones it wants
 * through the configuration; nothing here names a chip or a board):
 *  1. The link does not come up within link_timeout_ms: stage NO_LINK.
 *  2. An image is configured and fails its own checks (length, digest, chip,
 *     project, version): stage BAD_IMAGE, nothing is written.
 *  3. The coprocessor is not the configured chip, or speaks a protocol that
 *     is not accepted: stage UNSUPPORTED.
 *  4. The coprocessor already runs the image's firmware (project, version and
 *     ELF digest equal): the attempt marker is cleared, stage READY.
 *  5. No image is configured: stage READY (read-only check).
 *  6. The coprocessor runs the firmware named by `legacy` and no attempt is
 *     recorded: left alone, stage READY.
 *  7. An attempt marker is set and manual_retry is false: stage UNFINISHED.
 *  8. The coprocessor's firmware is in `sources` (project, version, ELF
 *     digest) and the image fits that entry's limit: update.
 *  9. The coprocessor speaks no version query at all and answers like the
 *     pre-versioned protocol (legacy_protocol_capability): update with the
 *     legacy transfer, image at most legacy_image_max.
 * 10. Anything else that reports a project and a version on an accepted
 *     protocol, with no attempt recorded: left alone, stage READY.
 * 11. Otherwise: stage UNSUPPORTED.
 *
 * Update: the attempt marker is stored (and flushed) before the first block;
 * blocks of write_block_bytes are sent with a yield after each; a block that
 * fails is never resent and the transfer is not finished (the coprocessor may
 * have written it): stage POWER_CYCLE. One transfer per boot. After the last
 * block: end, activate (the legacy transfer activates in end), wait
 * activate_wait_ms (legacy: legacy_activate_wait_ms), stage RESTARTING. The
 * marker is cleared by rule 4 on the next boot.
 *
 * The attempt marker is one unsigned 8-bit value in the kv port, value 1
 * while an update is unconfirmed. Its namespace and key are configuration so
 * that deployed devices keep theirs.
 *
 * Calling tasks: create is [board]; the hooks are [engine] (the network
 * engine's worker; never two at once, none while a link or session is up
 * except coprocessor_info). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A firmware image the board carries for its coprocessor. All pointers have
 * static storage. */
typedef struct {
    const uint8_t *data;
    size_t size;
    const char *sha256;         /* of data, 64 hexadecimal digits */
    const char *project;        /* application descriptor: project name */
    const char *version;        /* application descriptor: version */
} xiaotai_coprocessor_image_t;

/* A firmware the board may update from. */
typedef struct {
    const char *project;
    const char *version;
    const char *elf_sha256;     /* 64 hexadecimal digits; NULL: any build of this version */
    size_t image_max;           /* largest image verified to fit beside it */
} xiaotai_coprocessor_source_t;

typedef struct {
    uint32_t provisioning;      /* XIAOTAI_PROVISION_* of the network descriptor */
    /* Facts for the Wi-Fi driver; becomes the part's driver_config. Static
     * storage. NULL: the driver's defaults. */
    const xiaotai_network_wifi_t *wifi;
    uint16_t chip_id;          /* chip id in the image header the coprocessor must be */
    uint16_t chip_revision;     /* revision (major * 100 + minor) images must support; 0: not checked */

    /* The host link is started and stopped by prepare() instead of by the
     * system at boot; a manual retry restarts it. */
    bool manual_link;
    uint32_t link_timeout_ms;   /* 0: 15000 */

    /* Host-link protocol versions accepted, NULL-terminated ("3.0.7"). */
    const char *const *protocols;

    /* ---- automatic update at prepare(); image NULL: never write */
    const xiaotai_coprocessor_image_t *image;
    const xiaotai_coprocessor_source_t *sources;
    size_t source_count;
    xiaotai_coprocessor_source_t legacy;    /* project NULL: none; elf_sha256 and image_max unused */
    /* Capability word of the pre-versioned protocol; 0: rule 9 is off. */
    uint32_t legacy_protocol_capability;
    size_t legacy_image_max;
    uint32_t activate_wait_ms;              /* 0: 50 */
    uint32_t legacy_activate_wait_ms;       /* 0: 6000 */
    uint16_t write_block_bytes;             /* 0: 1024 */
    const char *attempt_space, *attempt_key;    /* kv namespace and key of the attempt marker */

    /* ---- coprocessor_update() with an image the caller obtained */
    const char *const *update_projects;     /* accepted project names, NULL-terminated; NULL: updates refused */
    size_t update_image_max;
    uint16_t update_block_bytes;            /* 0: 512 */
    uint32_t update_restart_wait_ms;        /* wait after activation before returning; 0: 3000 */

    /* ---- coprocessor_command(): commands sent on the coprocessor's data channel */
    uint32_t command_channel;               /* message id; 0: commands not supported */
    const char *const *commands;            /* accepted words, NULL-terminated */
    /* Commands of `commands` after which the coprocessor restarts with other
     * firmware (a rollback), NULL-terminated; NULL: none. For these the hook,
     * once the command was sent, waits restart_command_wait_ms before it
     * returns XIAOTAI_OK, so that the caller may restart the host at once (the
     * coprocessor must finish switching before the host resets the link). A
     * send that fails returns at once with its error. */
    const char *const *restart_commands;
    uint32_t restart_command_wait_ms;
} xiaotai_coprocessor_config_t;

/* [board] Creates the network part; touches no hardware. The configuration is
 * referenced, not copied: it has static storage. The object is allocated once
 * and never freed. Hooks the configuration does not enable are NULL in the
 * part. XIAOTAI_ERR_INVALID_ARG: a restart command that is not in commands. */
xiaotai_err_t xiaotai_coprocessor_create(const xiaotai_coprocessor_config_t *config,
                                         const xiaotai_network_t **network);

/* What prepare() found, for diagnostics after a final stage. */
typedef struct {
    xiaotai_network_stage_t stage;      /* last stage reported */
    xiaotai_err_t error;
    uint8_t rule;                       /* 1..11: the rule that decided; 0: prepare() has not run */
    bool attempt_recorded;
    bool transfer_started;              /* an update was started in this boot */
    bool legacy_protocol;
} xiaotai_coprocessor_state_t;

/* [any] Never blocks. */
void xiaotai_coprocessor_state(const xiaotai_network_t *network, xiaotai_coprocessor_state_t *state);

#ifdef __cplusplus
}
#endif
