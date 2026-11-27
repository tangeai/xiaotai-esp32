// SPDX-License-Identifier: MIT
#include <string.h>
#include "platform_internal.h"
#include "xiaotai/framework.h"
#include "xiaotai/json.h"
#include "xiaotai/port/sys.h"

#define PROFILE_RETRY_US INT64_C(15000000)

static struct {
    const char *name;
    xiaotai_platform_profile_fn_t write;
    void *ctx;
} scenes[8];
static size_t scene_count;
static uint32_t next_id;
/* An even value is a queued opaque ticket; the owner marks its turn odd
 * before it reads platform state or borrows the draft slot. */
static atomic_uint owner_turn;

size_t platform_profile_body_limit(void)
{
    /* A composed profile can be larger than the biggest individual module
     * request. Extend only the final existing slot, and only in PSRAM. */
    return platform.external && scene_count ? platform.config.request_body_bytes : platform.request_limit;
}

xiaotai_err_t xiaotai_platform_profile_scene(const char *scene, xiaotai_platform_profile_fn_t write, void *ctx)
{
    if (!scene || !*scene || !write || strnlen(scene, XIAOTAI_PLATFORM_SCENE_NAME_MAX) == XIAOTAI_PLATFORM_SCENE_NAME_MAX)
        return XIAOTAI_ERR_INVALID_ARG;
    if (!atomic_load(&platform.initialized) || atomic_load(&platform.started)) return XIAOTAI_ERR_INVALID_STATE;
    if (platform.provisioning) return XIAOTAI_OK;
    for (size_t i = 0; i < scene_count; ++i)
        if (!strcmp(scene, scenes[i].name)) return XIAOTAI_ERR_INVALID_STATE;
    if (scene_count >= sizeof(scenes) / sizeof(scenes[0])) return XIAOTAI_ERR_NO_MEM;
    scenes[scene_count].name = scene;
    scenes[scene_count].write = write;
    scenes[scene_count++].ctx = ctx;
    return XIAOTAI_OK;
}

size_t xiaotai_platform_scene_parts(const char *activity, const char *scene, char *out,
                                   size_t capacity, size_t *length)
{
    if (!activity || !scene || !out || !length || !*length || *length > capacity || !xiaotai_on_owner()) return 0;
    const void *parts[XIAOTAI_PLATFORM_SCENE_PARTS_MAX];
    const size_t total = xiaotai_activity_contributions(activity, XIAOTAI_PLATFORM_SLOT_SCENE, scene,
        parts, XIAOTAI_PLATFORM_SCENE_PARTS_MAX);
    const size_t count = total < XIAOTAI_PLATFORM_SCENE_PARTS_MAX ? total : XIAOTAI_PLATFORM_SCENE_PARTS_MAX;
    size_t dropped = total - count, appended = 0;
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_platform_scene_part_t *part = parts[i];
        const size_t comma = *length == 1 && out[0] == '{' ? 0U : 1U;
        const size_t remaining = capacity - *length;
        if (!part || !part->write || remaining < comma) { ++dropped; continue; }
        /* Only bytes before length are committed; the writer fills the
         * unused tail and the separator is committed on success. */
        const int n = part->write(out + *length + comma, remaining - comma, part->ctx);
        if (n < 0 || (size_t)n > remaining - comma) { ++dropped; continue; }
        if (!n) continue;
        if (comma) out[*length] = ',';
        *length += comma + (size_t)n;
        ++appended;
    }
    if (dropped && platform.objects) {
        platform_lock();
        platform.stats.profile_parts_dropped += (uint32_t)dropped;
        platform_unlock();
    }
    return appended;
}

int platform_profile_document(char *out, size_t capacity)
{
    xiaotai_json_writer_t json;
    xiaotai_json_writer_init(&json, out, capacity);
    xiaotai_json_object_begin(&json, NULL);
    if (platform.config.chip_model && platform.config.board_model) {
        xiaotai_json_object_begin(&json, "hardware");
        xiaotai_json_put_string(&json, "chip_model", platform.config.chip_model);
        xiaotai_json_put_string(&json, "board_model", platform.config.board_model);
        xiaotai_json_object_end(&json);
    }
    if (!platform.config.profile_omit_firmware_version) {
        xiaotai_firmware_t firmware;
        xiaotai_firmware(&firmware);
        xiaotai_json_put_string(&json, "firmware_version", firmware.version ? firmware.version : "unknown");
    }
    xiaotai_json_object_begin(&json, "profiles");
    xiaotai_json_object_end(&json);
    xiaotai_json_object_end(&json);
    size_t used;
    if (xiaotai_json_writer_end(&json, &used) != XIAOTAI_OK) return -1;
    used -= 2U; /* Replace the two closing braces after the scene writers. */
    bool comma = false;
    for (size_t i = 0; i < scene_count; ++i) {
        /* Use the JSON writer for escaping the key. The scene's potentially
         * large value is written directly into the reserved request slot. */
        char key[6U * XIAOTAI_PLATFORM_SCENE_NAME_MAX + sizeof("{\"\":null}")];
        size_t key_size;
        xiaotai_json_writer_init(&json, key, sizeof(key));
        xiaotai_json_object_begin(&json, NULL);
        xiaotai_json_put_null(&json, scenes[i].name);
        xiaotai_json_object_end(&json);
        if (xiaotai_json_writer_end(&json, &key_size) != XIAOTAI_OK) return -1;
        key_size -= sizeof("{null}") - 1U; /* The middle is just the quoted name and colon. */
        const size_t offset = used + (comma ? 1U : 0U) + key_size;
        if (offset >= capacity - 2U) return -1;
        const size_t room = capacity - offset - 2U; /* One byte also lets writers append a NUL. */
        const int n = scenes[i].write(out + offset, room, scenes[i].ctx);
        if (n < 0 || (size_t)n >= room || (n && (n < 2 || out[offset] != '{' || out[offset + (size_t)n - 1U] != '}')))
            return -1;
        if (!n) continue;
        if (comma) out[used++] = ',';
        memcpy(out + used, key + 1U, key_size);
        used = offset + (size_t)n;
        comma = true;
    }
    memcpy(out + used, "}}", sizeof("}}"));
    return (int)(used + 2U);
}

static bool online(void)
{
    return !atomic_load(&platform.stopped) && platform.status.phase == XIAOTAI_PLATFORM_ONLINE &&
        platform.network_up && platform.status.signaling &&
        platform.status.generation == xiaotai_identity_generation();
}

bool platform_profile_current(uint32_t id)
{
    return platform.profile.id == id && platform.profile.phase == PROFILE_SENDING && online();
}

bool platform_profile_hold_blocks(uint32_t id)
{
    if (!id || id != platform.profile.id || !platform.status.held) return false;
    platform.profile.held_before_wire = true;
    return true;
}

static void pending(bool value)
{
    platform.profile_pending = value;
    platform_count_publish();
}

static bool permitted(void) { return !platform.config.profile_requires_ready || platform.profile_ready_input; }

void platform_profile_online(void)
{
    next_id += 2U;
    if (!next_id) next_id = 2U;
    platform.profile = (platform_profile_t){.phase = PROFILE_WAIT, .id = next_id};
    pending(permitted());
}

void platform_profile_offline(void)
{
    if (!platform.profile.id) return;
    platform.profile = (platform_profile_t){0};
    pending(false);
}

void xiaotai_platform_profile_refresh(void)
{
    if (!platform.objects || !xiaotai_on_owner()) return;
    platform_lock();
    if (online() && platform.profile.phase != PROFILE_SENDING && platform.profile.phase != PROFILE_POSTED)
        platform_profile_online();
    platform_unlock();
    platform_wake();
}

void xiaotai_platform_profile_ready(bool ready)
{
    if (!platform.config.profile_requires_ready || !platform.objects || !xiaotai_on_owner()) return;
    platform_lock();
    if (platform.profile_ready_input != ready) {
        platform.profile_ready_input = ready;
        platform_profile_t *p = &platform.profile;
        if (ready && online() && p->phase != PROFILE_SENDING) platform_profile_online();
        else if (!ready && (p->phase == PROFILE_WAIT || p->phase == PROFILE_POSTED)) {
            p->phase = PROFILE_WAIT;
            pending(false);
        }
    }
    platform_unlock();
    platform_wake();
}

static void finish(bool ok, bool retry)
{
    platform_profile_t *p = &platform.profile;
    if (!ok) ++platform.stats.profile_failures;
    if (retry && p->attempts < platform.config.profile_attempts) {
        p->phase = PROFILE_WAIT;
        p->due = xiaotai_time_us() + PROFILE_RETRY_US;
        if (!permitted()) pending(false);
        return;
    }
    if (ok) platform.status.profile_ready = true;
    p->phase = PROFILE_IDLE;
    pending(false);
    platform_event_publish(PLATFORM_EVENT_PROFILE, ok ? 1 : 0);
}

static void postpone(void)
{
    --platform.profile.attempts;
    platform.profile.phase = PROFILE_WAIT;
    platform.profile.due = 0;
    platform.profile.held_before_wire = false;
    if (!permitted()) pending(false);
}

static void reply(const xiaotai_platform_reply_t *r, void *ctx)
{
    const uint32_t id = (uint32_t)(uintptr_t)ctx;
    const bool valid = r->result == XIAOTAI_OK;
    const int32_t code = r->code;
    const int status = r->http_status;
    const bool unbound = valid && status == 410 && code == XIAOTAI_PLATFORM_CODE_UNBOUND;
    platform_lock();
    const bool current = platform_profile_current(id);
    if (current) {
        if ((r->result == XIAOTAI_ERR_PLATFORM_GATED && platform.profile.held_before_wire) ||
            (r->result == XIAOTAI_ERR_BUSY && platform.config.defer_background_while_active)) postpone();
        else {
            const bool ok = valid && status == 200 && code == 200;
            const bool retry = !status || status == 429 || (status >= 500 && status < 600) ||
                (status == 200 && platform.config.profile_retry_business_errors &&
                 (code == XIAOTAI_PLATFORM_NO_CODE || code == 50000));
            finish(ok, retry);
        }
    }
    platform_unlock();
    if (current && unbound) platform_binding_request_lost(r->generation);
    platform_wake();
}

static void owner_report(void *ctx)
{
    const uint32_t id = (uint32_t)(uintptr_t)ctx;
    uint32_t expected = id;
    if (!atomic_compare_exchange_strong(&owner_turn, &expected, id | 1U)) return;
    platform_lock();
    platform_profile_t *p = &platform.profile;
    const bool current = p->id == id && p->phase == PROFILE_POSTED && online();
    const bool admitted = current && permitted() && !platform.status.held;
    if (admitted) {
        p->phase = PROFILE_SENDING;
        ++p->attempts;
    } else if (current) p->phase = PROFILE_WAIT;
    platform_unlock();
    if (admitted) {
        const xiaotai_err_t err = platform_profile_request(id, reply);
        if (err != XIAOTAI_OK) {
            platform_lock();
            if (platform_profile_current(id)) {
                if (err == XIAOTAI_ERR_BUSY || err == XIAOTAI_ERR_PLATFORM_GATED) postpone();
                else finish(false, err == XIAOTAI_ERR_NO_MEM);
            }
            platform_unlock();
        }
    }
    platform_wake();
    atomic_store(&owner_turn, 0);
}

uint32_t platform_profile_step(void)
{
    platform_lock();
    uint32_t wait = XIAOTAI_WAIT_FOREVER;
    platform_profile_t *p = &platform.profile;
    if (p->id && !online()) platform_profile_offline();
    if (p->phase == PROFILE_WAIT && permitted() && !platform.status.held && !platform.gate &&
        !(platform.event_pending & (1U << PLATFORM_EVENT_ONLINE))) {
        const bool due = !p->due || !(wait = platform_remaining_ms(p->due));
        if (due) {
            if (xiaotai_platform_background_deferred()) {
                if (!p->deferred) ++platform.stats.profile_deferrals;
                p->deferred = true;
                wait = XIAOTAI_WAIT_FOREVER;
            } else if (!atomic_load(&owner_turn) && !p->in_slot) {
                p->deferred = false;
                atomic_store(&owner_turn, p->id);
                if (xiaotai_run(owner_report, (void *)(uintptr_t)p->id) == XIAOTAI_OK) {
                    p->phase = PROFILE_POSTED;
                    wait = XIAOTAI_WAIT_FOREVER;
                } else {
                    atomic_store(&owner_turn, 0);
                    ++platform.stats.event_post_retries;
                    wait = PLATFORM_OWNER_REPOST_MS;
                }
            } else wait = PLATFORM_OWNER_REPOST_MS;
        }
    }
    platform_unlock();
    return wait;
}

void platform_profile_reset(void)
{
    for (;;) {
        uint32_t current = atomic_load(&owner_turn);
        if (current & 1U) xiaotai_delay_ms(1);
        else if (atomic_compare_exchange_strong(&owner_turn, &current, 0U)) break;
    }
    scene_count = 0;
}
