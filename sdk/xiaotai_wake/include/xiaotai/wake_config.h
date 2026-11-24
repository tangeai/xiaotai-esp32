// SPDX-License-Identifier: MIT
#pragma once
/* Build-time options of the wake-word engine (xiaotai/config.h explains the
 * mechanism; the options themselves are in the component's Kconfig). */
#include "xiaotai/config.h"

/* Embed the built-in wake model and register it during init. Off: the engine
 * has no model until one is registered with xiaotai_wake_model_register (host
 * tests, a product with a model of its own). */
#define XIAOTAI_WAKE_BUILTIN_MODEL XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_WAKE_BUILTIN_MODEL, 0)

/* Target facts of the built-in model's backend (esp-idf/Kconfig.platform holds
 * the per-target defaults). The host has neither the inference library's
 * kernels nor the DSP library: no fast path, and the portable FFT. */
/* The dilated depthwise fast path patched into the inference library; the
 * build applies the patch only when this is on. */
#define XIAOTAI_WAKE_DILATED_FAST_PATH XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_WAKE_DILATED_FAST_PATH, 0)
/* The feature front end uses the portable C FFT instead of the optimized one. */
#define XIAOTAI_WAKE_FFT_PORTABLE XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_WAKE_FFT_PORTABLE, 1)
