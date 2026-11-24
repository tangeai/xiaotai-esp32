// SPDX-License-Identifier: MIT
#pragma once
/* Hardware needs: the one vocabulary in which engines, base modules and
 * applications say what hardware they require and a board says what it has.
 *
 * The board's parts are what satisfy a need: xiaotai_hal_needs()
 * (xiaotai/hal.h) derives the board's bits from its part descriptors. The
 * runtime framework (xiaotai/framework.h) compares the two before anything is
 * initialized, and the build compares them before anything is compiled: the
 * manifests (engine.json, module.json, application.json) name the same needs
 * in lower case ("audio_in", ..., "door_lock"). The header is published by the
 * port component, on which everything is built, so the hardware abstraction
 * and the framework share it without depending on each other.
 *
 * Source of truth: sdk/xiaotai_hal/needs.json holds each need's name, bit and
 * the part and descriptor values that satisfy it. This header, the
 * derivation in xiaotai_hal_needs() and the build's table follow it, and
 * scripts/check_sdk_layers.py fails when they disagree. A new need is one
 * entry there and one line here, with the next bit.
 *
 * Bits never change once given, because generated tables and stored board
 * manifests carry them. Manifests name needs, never bits, so the run-time
 * mask can widen without touching a manifest: the enumerators are ints and
 * end at bit 30; the need after that turns these into 64-bit constants and
 * the needs fields of xiaotai/framework.h, xiaotai/selection.h and
 * xiaotai_hal_needs() into uint64_t in one change. */

enum {
    XIAOTAI_NEED_AUDIO_IN = 1u << 0,      /* any audio input */
    XIAOTAI_NEED_PCM_IN = 1u << 1,        /* PCM audio input (wake word, echo cancellation) */
    XIAOTAI_NEED_AUDIO_OUT = 1u << 2,
    XIAOTAI_NEED_DISPLAY = 1u << 3,
    XIAOTAI_NEED_TOUCH = 1u << 4,         /* a display with a pointer */
    XIAOTAI_NEED_KEY = 1u << 5,           /* a button input */
    XIAOTAI_NEED_LIGHT = 1u << 6,
    XIAOTAI_NEED_RELAY = 1u << 7,
    XIAOTAI_NEED_CAMERA = 1u << 8,
    XIAOTAI_NEED_NETWORK = 1u << 9,
    XIAOTAI_NEED_EXTERNAL_RAM = 1u << 10, /* as the memory port reports it */
    XIAOTAI_NEED_ENCODER = 1u << 11,      /* a rotary input */
    XIAOTAI_NEED_CARD = 1u << 12,         /* a card reader input */
    XIAOTAI_NEED_VOICE_COMMAND = 1u << 13, /* an input that recognizes spoken commands itself */
    XIAOTAI_NEED_CONTACT = 1u << 14,      /* a contact input (a door contact) */
    XIAOTAI_NEED_VIBRATOR = 1u << 15,
    /* A relay in the "door" role family (a role "door" or "door.<name>"),
     * the output a door lock drives; a relay with another role satisfies
     * only XIAOTAI_NEED_RELAY. */
    XIAOTAI_NEED_DOOR_LOCK = 1u << 16,
};
