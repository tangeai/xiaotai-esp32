// SPDX-License-Identifier: MIT
/* Diagnostics section "settings" (AT+STATUS settings). Two records:
 *   (no prefix; the console writes +SETTINGS:)
 *     volume= speaker_muted= mic_muted= mic_gain= gain_min= gain_max= guidance=
 *     sleep= previewing= preview= apply_error= save_pending= revision=
 *   +SETTINGS-STATS: changes= applied= refused= steps= steps_at_limit=
 *     previews= not_saved= output_errors= late_kept= take_back_deferred=
 * A setting this board or build does not register is written as '-'. */
#include <stdio.h>
#include "settings_internal.h"

#define S xiaotai_settings_state

static void number(char *out, size_t size, bool present, long value)
{
    if (present) snprintf(out, size, "%ld", value);
    else snprintf(out, size, "-");
}

void xiaotai_settings_diag_report(const xiaotai_reply_t *out)
{
    xiaotai_settings_status_t status;
    xiaotai_settings_status(&status);
    const bool guidance = atomic_load(&S.has_guidance);
    /* Sized for any long, so the formatting can never truncate; the whole
     * report stays far inside the handler's stack budget. */
    char volume[24], speaker[24], mic[24], gain[24], low[24], high[24], hint[24], sleep[24], preview[24];
    number(volume, sizeof(volume), status.has_speaker, status.volume);
    number(speaker, sizeof(speaker), status.has_speaker, status.speaker_muted);
    number(mic, sizeof(mic), status.has_microphone, status.mic_muted);
    number(gain, sizeof(gain), status.has_mic_gain, status.mic_gain_db);
    number(low, sizeof(low), status.has_mic_gain, status.mic_gain_min_db);
    number(high, sizeof(high), status.has_mic_gain, status.mic_gain_max_db);
    number(hint, sizeof(hint), guidance, status.guidance);
    number(sleep, sizeof(sleep), status.has_screen, (long)status.screen_sleep_s);
    number(preview, sizeof(preview), status.previewing, status.preview_volume);
    char line[512];
    snprintf(line, sizeof(line),
             "volume=%s speaker_muted=%s mic_muted=%s mic_gain=%s gain_min=%s gain_max=%s guidance=%s sleep=%s "
             "previewing=%d preview=%s apply_error=%d save_pending=%d revision=%lu",
             volume, speaker, mic, gain, low, high, hint, sleep, status.previewing ? 1 : 0, preview,
             status.apply_error, status.save_pending ? 1 : 0, (unsigned long)status.revision);
    out->write(out->ctx, line);
    settings_counters_t *c = &S.counters;
    snprintf(line, sizeof(line),
             "+SETTINGS-STATS: changes=%lu applied=%lu refused=%lu steps=%lu steps_at_limit=%lu previews=%lu "
             "not_saved=%lu output_errors=%lu late_kept=%lu take_back_deferred=%lu",
             (unsigned long)atomic_load(&c->changes), (unsigned long)atomic_load(&c->applied),
             (unsigned long)atomic_load(&c->refused), (unsigned long)atomic_load(&c->steps),
             (unsigned long)atomic_load(&c->steps_at_limit), (unsigned long)atomic_load(&c->previews),
             (unsigned long)atomic_load(&c->not_saved), (unsigned long)atomic_load(&c->output_errors),
             (unsigned long)atomic_load(&c->late_kept), (unsigned long)atomic_load(&c->take_back_deferred));
    out->write(out->ctx, line);
}
