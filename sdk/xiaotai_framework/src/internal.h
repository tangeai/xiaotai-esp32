// SPDX-License-Identifier: MIT
#pragma once
/* Framework internals shared by its source files. */
#include "xiaotai/framework.h"
#include "xiaotai/port/os.h"

#define FW_START_WAIT_MS 60000U
/* Restart steps poll guards and holds at this period on the owner task, so a
 * release from any task needs no message (a full queue cannot lose it). */
#define FW_RESTART_POLL_MS 50U
#define FW_RESTART_FLUSH_MS 3000U

/* Registrations are only accepted while modules and applications initialize. */
bool fw_initializing(void);
bool fw_started(void);

/* The application whose code runs on the owner task (index into the
 * configuration's applications), -1 for an engine or module, and -1 off the
 * owner task. A registration records it, and the framework enters it again
 * around every call back into that code, so xiaotai_view_show() knows who
 * draws. fw_app_enter returns the previous value for fw_app_leave. */
int16_t fw_app_current(void);
int16_t fw_app_enter(int16_t app);
void fw_app_leave(int16_t previous);

/* Settings and commands keep their own registries; core resets them. */
void fw_settings_reset(void);
/* Preserve the event itself when its optional metadata cannot be allocated. */
xiaotai_err_t fw_event_emit_optional(const char *name, const xiaotai_args_t *args, const void *data, size_t size);
void fw_commands_reset(void);
/* Registers without the initializing check: the framework's own section,
 * before any module initializes. */
xiaotai_err_t fw_diag_add(const char *name, xiaotai_diag_fn_t report);
/* The setting store's flush, if it has one; XIAOTAI_OK otherwise. */
xiaotai_err_t fw_settings_flush(uint32_t timeout_ms);
/* Counted setting refusals while quiesced, for the diagnostics. */
uint32_t fw_settings_refused(void);
