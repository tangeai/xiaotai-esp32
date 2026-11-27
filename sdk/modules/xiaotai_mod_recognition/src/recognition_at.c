// SPDX-License-Identifier: MIT
#include "recognition_internal.h"
#include "xiaotai/at_command.h"

#include <inttypes.h>

#define ID_MAX (XIAOTAI_RECOGNITION_ID_MAX - 1U)

static const char *printable_id(const char *id)
{
    return xiaotai_at_is_id(id, ID_MAX) ? id : "";
}

static xiaotai_err_t show_status(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)argv;
    static const char *const phases[] = {"idle", "config", "recording", "upload", "cancelling", "accepted",
        "recovery", "failed", "cancelled", "query", "result", "refresh", "download", "playback"};
    xiaotai_recognition_status_t status;
    const xiaotai_err_t err = xiaotai_recognition_status(&status);
    if (err != XIAOTAI_OK) return err;
    const unsigned phase = (unsigned)status.phase;
    const char *name = phase < sizeof(phases) / sizeof(*phases) ? phases[phase] : "invalid";
    xiaotai_at_printf(out, "+RECOGNITION-STATE: phase=%s token=%" PRIu32 " samples=%" PRIu32 " target=%" PRIu32
                     " error=%d http=%d code=%" PRId32 " cleanup=%d possibly_sent=%d request=%s",
                     name, status.token, status.samples, status.target_samples, status.error, status.http_status,
                     status.code, status.cleanup_pending, status.possibly_sent, printable_id(status.request_id));
    xiaotai_at_printf(out, "+RECOGNITION-TASK: revision=%" PRIu64 " state=%d items=%u rejected_without_task=%d",
                     status.task_revision, (int)status.task_state, (unsigned)status.task_items,
                     status.rejected_without_task);
    xiaotai_at_printf(out, "+RECOGNITION-POLICY: revision=%" PRIu64 " enabled=%d error=%d http=%d code=%" PRId32,
                     status.config_revision, status.config_enabled, status.config_error,
                     status.config_http_status, status.config_code);
    xiaotai_at_printf(out, "+RECOGNITION-PLAYBACK: item=%s consumed=%d playback_ms=%" PRIu32
                     " playback_total_ms=%" PRIu32 " paused=%d", printable_id(status.item_id),
                     (int)status.consumption, status.playback_ms, status.playback_total_ms, status.playback_paused);
    xiaotai_at_printf(out, "+RECOGNITION-DECISION: serial=%" PRIu32 " action=%d error=%d",
                     status.decision_serial, (int)status.decision_action, status.decision_error);
    return XIAOTAI_OK;
}

static void diagnose(const xiaotai_reply_t *out)
{
    xiaotai_recognition_stats_t stats;
    xiaotai_recognition_stats(&stats);
#define COUNT(name) " " #name "=%" PRIu32
    xiaotai_at_printf(out, "+RECOGNITION-COUNTS:" COUNT(recordings) COUNT(recordings_completed) COUNT(recordings_failed)
                     COUNT(submissions) COUNT(submissions_accepted) COUNT(submissions_uncertain)
                     COUNT(submissions_conflict) COUNT(queries) COUNT(query_failures) COUNT(query_retries)
                     COUNT(policy_reads) COUNT(policy_failures) COUNT(recordings_spilled), stats.recordings, stats.recordings_completed,
                     stats.recordings_failed, stats.submissions, stats.submissions_accepted,
                     stats.submissions_uncertain, stats.submissions_conflict, stats.queries, stats.query_failures,
                     stats.query_retries, stats.policy_reads, stats.policy_failures, stats.recordings_spilled);
    xiaotai_at_printf(out, "+RECOGNITION-RESULTS:" COUNT(notifications) COUNT(notifications_ignored)
                     COUNT(results_adopted) COUNT(results_stale) COUNT(downloads) COUNT(download_failures)
                     COUNT(playbacks) COUNT(playbacks_completed) COUNT(playbacks_cut), stats.notifications,
                     stats.notifications_ignored, stats.results_adopted, stats.results_stale, stats.downloads,
                     stats.download_failures, stats.playbacks, stats.playbacks_completed, stats.playbacks_cut);
    xiaotai_at_printf(out, "+RECOGNITION-OWNER:" COUNT(decisions) COUNT(decisions_refused) COUNT(cancels) COUNT(revoked)
                     COUNT(activity_busy) COUNT(journal_writes) COUNT(journal_failures) COUNT(journal_timeouts)
                     COUNT(scope_advances) COUNT(stale) COUNT(release_retries), stats.decisions, stats.decisions_refused,
                     stats.cancels, stats.revoked, stats.activity_busy, stats.journal_writes, stats.journal_failures,
                     stats.journal_timeouts, stats.scope_advances, stats.stale, stats.release_retries);
#undef COUNT
}

static xiaotai_err_t record(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)out;
    uint32_t seconds;
    if (!xiaotai_at_parse_u32(argv[0], 1U, XIAOTAI_RECOGNITION_SECONDS_MAX, &seconds))
        return XIAOTAI_ERR_INVALID_ARG;
    return xiaotai_recognition_record(seconds);
}

static xiaotai_err_t cancel(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)argv;
    (void)out;
    return xiaotai_recognition_cancel(0);
}

static xiaotai_err_t query(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)argv;
    (void)out;
    return xiaotai_recognition_query();
}

static xiaotai_err_t resume(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)out;
    if (!xiaotai_at_is_id(argv[0], ID_MAX)) return XIAOTAI_ERR_INVALID_ARG;
    return xiaotai_recognition_resume(argv[0]);
}

static xiaotai_err_t dismiss(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)out;
    if (!xiaotai_at_is_id(argv[0], ID_MAX) || !xiaotai_at_is_id(argv[1], ID_MAX)) return XIAOTAI_ERR_INVALID_ARG;
    return xiaotai_recognition_dismiss(argv[0], argv[1]);
}

static xiaotai_err_t archive(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)out;
    uint64_t revision;
    if (!xiaotai_at_is_id(argv[0], ID_MAX) ||
        !xiaotai_at_parse_u64(argv[1], XIAOTAI_RECOGNITION_REVISION_MAX, &revision)) return XIAOTAI_ERR_INVALID_ARG;
    return xiaotai_recognition_archive(argv[0], revision);
}

xiaotai_err_t rc_at_init(void)
{
    static const xiaotai_at_verb_t verbs[] = {
        {.run = show_status},
        {.word = "RECORD", .min_args = 1, .max_args = 1, .run = record, .usage = "<seconds:1..60>"},
        {.word = "CANCEL", .run = cancel}, {.word = "QUERY", .run = query},
        {.word = "RESUME", .min_args = 1, .max_args = 1, .run = resume, .usage = "<request-id>"},
        {.word = "DISMISS", .min_args = 2, .max_args = 2, .run = dismiss, .usage = "<request-id> <item-id>"},
        {.word = "ARCHIVE", .min_args = 2, .max_args = 2, .run = archive, .usage = "<request-id> <revision>"},
    };
    const xiaotai_err_t err = xiaotai_at_verbs_register("AT+RECOGNITION", verbs, sizeof(verbs) / sizeof(*verbs));
    return err == XIAOTAI_OK ? xiaotai_diag_register("recognition", diagnose) : err;
}
