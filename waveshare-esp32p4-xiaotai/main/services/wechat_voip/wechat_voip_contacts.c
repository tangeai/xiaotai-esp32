#include "wechat_voip_contacts.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "app_memory_policy.h"
#include "tirtc_session.h"

static const char *TAG = "wx_voip_contacts";

typedef struct {
    SemaphoreHandle_t lock;
    char device_id[TIRTC_SESSION_DEVICE_ID_MAX_LEN];
    wechat_voip_auth_user_t cached_auth;
    wechat_voip_auth_user_t contacts[WECHAT_VOIP_CONTACT_MAX];
    uint8_t contact_count;
    bool ready;
    bool server_synced;
    esp_err_t sync_error;
} wechat_voip_contacts_runtime_t;

static EXT_RAM_BSS_ATTR wechat_voip_contacts_runtime_t s_contacts;
static portMUX_TYPE s_contacts_init_lock = portMUX_INITIALIZER_UNLOCKED;

static void copy_str(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strlcpy(dst, src, dst_size);
}

static bool str_same(const char *a, const char *b)
{
    return strcmp(a != NULL ? a : "", b != NULL ? b : "") == 0;
}

bool wechat_voip_remark_is_valid(const char *remark)
{
    if (remark == NULL) {
        return false;
    }

    const unsigned char *cursor = (const unsigned char *)remark;
    size_t remaining = strlen(remark);
    size_t characters = 0U;
    while (remaining > 0U) {
        size_t width = 0U;
        if (cursor[0] <= 0x7FU) {
            width = 1U;
        } else if (cursor[0] >= 0xC2U && cursor[0] <= 0xDFU) {
            if (remaining < 2U || (cursor[1] & 0xC0U) != 0x80U) {
                return false;
            }
            width = 2U;
        } else if (cursor[0] >= 0xE0U && cursor[0] <= 0xEFU) {
            if (remaining < 3U ||
                (cursor[1] & 0xC0U) != 0x80U || (cursor[2] & 0xC0U) != 0x80U ||
                (cursor[0] == 0xE0U && cursor[1] < 0xA0U) ||
                (cursor[0] == 0xEDU && cursor[1] >= 0xA0U)) {
                return false;
            }
            width = 3U;
        } else if (cursor[0] >= 0xF0U && cursor[0] <= 0xF4U) {
            if (remaining < 4U ||
                (cursor[1] & 0xC0U) != 0x80U || (cursor[2] & 0xC0U) != 0x80U ||
                (cursor[3] & 0xC0U) != 0x80U ||
                (cursor[0] == 0xF0U && cursor[1] < 0x90U) ||
                (cursor[0] == 0xF4U && cursor[1] > 0x8FU)) {
                return false;
            }
            width = 4U;
        } else {
            return false;
        }

        cursor += width;
        remaining -= width;
        if (++characters > WECHAT_VOIP_REMARK_MAX_CHARS) {
            return false;
        }
    }
    return true;
}

static void copy_remark(char *dst, size_t dst_size, const char *src)
{
    copy_str(dst, dst_size, wechat_voip_remark_is_valid(src) ? src : "");
}

static bool contact_valid(const wechat_voip_auth_user_t *entry)
{
    return entry != NULL && entry->openid[0] != '\0' && entry->model_id[0] != '\0';
}

static bool auth_user_same(const wechat_voip_auth_user_t *a,
                           const wechat_voip_auth_user_t *b)
{
    return str_same(a != NULL ? a->openid : NULL, b != NULL ? b->openid : NULL) &&
           str_same(a != NULL ? a->model_id : NULL, b != NULL ? b->model_id : NULL) &&
           str_same(a != NULL ? a->app_id : NULL, b != NULL ? b->app_id : NULL) &&
           str_same(a != NULL ? a->remark : NULL, b != NULL ? b->remark : NULL);
}

static bool remember_locked(const wechat_voip_auth_user_t *user)
{
    int existing = -1;
    uint8_t count = s_contacts.contact_count > WECHAT_VOIP_CONTACT_MAX ?
                    WECHAT_VOIP_CONTACT_MAX :
                    s_contacts.contact_count;

    for (uint8_t index = 0; index < count; ++index) {
        if (str_same(s_contacts.contacts[index].openid, user->openid)) {
            existing = (int)index;
            break;
        }
    }

    wechat_voip_auth_user_t next = {0};
    copy_str(next.openid, sizeof(next.openid), user->openid);
    copy_str(next.model_id,
             sizeof(next.model_id),
             user->model_id[0] != '\0' ? user->model_id :
             existing >= 0 ? s_contacts.contacts[existing].model_id : "");
    copy_str(next.app_id,
             sizeof(next.app_id),
             user->app_id[0] != '\0' ? user->app_id :
             existing >= 0 ? s_contacts.contacts[existing].app_id : "");
    copy_remark(next.remark,
                sizeof(next.remark),
                user->remark[0] != '\0' ? user->remark :
                existing >= 0 ? s_contacts.contacts[existing].remark : "");

    bool changed = true;
    if (existing >= 0) {
        changed = !auth_user_same(&s_contacts.contacts[existing], &next);
    } else if (count < WECHAT_VOIP_CONTACT_MAX) {
        existing = count++;
        s_contacts.contact_count = count;
    } else {
        memmove(&s_contacts.contacts[1],
                &s_contacts.contacts[0],
                sizeof(s_contacts.contacts[0]) * (WECHAT_VOIP_CONTACT_MAX - 1U));
        existing = 0;
        s_contacts.contact_count = WECHAT_VOIP_CONTACT_MAX;
    }

    s_contacts.contacts[existing] = next;
    return changed;
}

esp_err_t wechat_voip_contacts_init(void)
{
    SemaphoreHandle_t created_lock = NULL;
    bool ready = false;

    portENTER_CRITICAL(&s_contacts_init_lock);
    ready = s_contacts.lock != NULL;
    portEXIT_CRITICAL(&s_contacts_init_lock);

    if (!ready) {
        created_lock =
            xSemaphoreCreateMutexWithCaps(APP_SYNC_CAPS_CONTROL);
        if (created_lock == NULL) {
            portENTER_CRITICAL(&s_contacts_init_lock);
            ready = s_contacts.lock != NULL;
            portEXIT_CRITICAL(&s_contacts_init_lock);
            return ready ? ESP_OK : ESP_ERR_NO_MEM;
        }

        portENTER_CRITICAL(&s_contacts_init_lock);
        if (s_contacts.lock == NULL) {
            s_contacts.lock = created_lock;
            created_lock = NULL;
        }
        portEXIT_CRITICAL(&s_contacts_init_lock);

        if (created_lock != NULL) {
            vSemaphoreDeleteWithCaps(created_lock);
        }
    }
    return ESP_OK;
}

void wechat_voip_contacts_reset_for_device(const char *device_id)
{
    if (wechat_voip_contacts_init() != ESP_OK) {
        return;
    }

    xSemaphoreTake(s_contacts.lock, portMAX_DELAY);
    if (!str_same(s_contacts.device_id, device_id)) {
        memset(s_contacts.contacts, 0, sizeof(s_contacts.contacts));
        memset(&s_contacts.cached_auth, 0, sizeof(s_contacts.cached_auth));
        s_contacts.contact_count = 0;
        s_contacts.server_synced = false;
        s_contacts.sync_error = ESP_OK;
        copy_str(s_contacts.device_id, sizeof(s_contacts.device_id), device_id);
    }
    s_contacts.ready = true;
    xSemaphoreGive(s_contacts.lock);
}

void wechat_voip_contacts_load(const char *device_id)
{
    /*
     * Contacts are an online authorization view. Keep only the current
     * in-memory snapshot and let each service entry refresh it from server.
     */
    wechat_voip_contacts_reset_for_device(device_id);
}

void wechat_voip_contacts_replace(const wechat_voip_auth_user_t *users,
                                  size_t user_count,
                                  const char *source)
{
    wechat_voip_auth_user_t next[WECHAT_VOIP_CONTACT_MAX] = {0};
    uint8_t next_count = 0;

    if (wechat_voip_contacts_init() != ESP_OK) {
        return;
    }

    for (size_t index = 0;
         users != NULL && index < user_count && next_count < WECHAT_VOIP_CONTACT_MAX;
         ++index) {
        if (!contact_valid(&users[index])) {
            continue;
        }

        bool duplicate = false;
        for (uint8_t saved = 0; saved < next_count; ++saved) {
            if (str_same(next[saved].openid, users[index].openid)) {
                next[saved] = users[index];
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            next[next_count++] = users[index];
        }
    }

    xSemaphoreTake(s_contacts.lock, portMAX_DELAY);
    memset(s_contacts.contacts, 0, sizeof(s_contacts.contacts));
    memset(&s_contacts.cached_auth, 0, sizeof(s_contacts.cached_auth));
    memcpy(s_contacts.contacts, next, sizeof(next));
    s_contacts.contact_count = next_count;
    s_contacts.ready = true;
    s_contacts.server_synced = true;
    s_contacts.sync_error = ESP_OK;
    if (next_count > 0) {
        s_contacts.cached_auth = next[0];
    }
    xSemaphoreGive(s_contacts.lock);

    ESP_LOGD(TAG,
             "contact snapshot replaced: source=%s count=%u",
             source != NULL ? source : "refresh",
             (unsigned)next_count);
}

void wechat_voip_contacts_note_sync_error(esp_err_t error)
{
    if (error == ESP_OK || wechat_voip_contacts_init() != ESP_OK) {
        return;
    }
    xSemaphoreTake(s_contacts.lock, portMAX_DELAY);
    s_contacts.sync_error = error;
    xSemaphoreGive(s_contacts.lock);
}

bool wechat_voip_contacts_remember(const wechat_voip_auth_user_t *user,
                                   const char *source)
{
    if (user == NULL || user->openid[0] == '\0' ||
        wechat_voip_contacts_init() != ESP_OK) {
        return false;
    }
    if (user->model_id[0] == '\0') {
        ESP_LOGD(TAG,
                 "skip contact without model_id: source=%s openid_len=%u",
                 source != NULL ? source : "unknown",
                 (unsigned)strlen(user->openid));
        return false;
    }

    xSemaphoreTake(s_contacts.lock, portMAX_DELAY);
    wechat_voip_auth_user_t before = s_contacts.cached_auth;
    s_contacts.cached_auth = *user;
    bool changed = !auth_user_same(&before, &s_contacts.cached_auth);
    changed = remember_locked(user) || changed;
    xSemaphoreGive(s_contacts.lock);

    if (changed) {
        ESP_LOGD(TAG,
                 "contact cached in memory: source=%s openid_len=%u model_id_len=%u",
                 source != NULL ? source : "unknown",
                 (unsigned)strlen(user->openid),
                 (unsigned)strlen(user->model_id));
    }
    return changed;
}

bool wechat_voip_contacts_remove(const char *openid,
                                 wechat_voip_auth_user_t *removed)
{
    if (openid == NULL || openid[0] == '\0' ||
        wechat_voip_contacts_init() != ESP_OK) {
        return false;
    }

    bool found = false;
    xSemaphoreTake(s_contacts.lock, portMAX_DELAY);
    uint8_t count = s_contacts.contact_count > WECHAT_VOIP_CONTACT_MAX ?
                    WECHAT_VOIP_CONTACT_MAX :
                    s_contacts.contact_count;
    for (uint8_t index = 0; index < count; ++index) {
        if (!str_same(s_contacts.contacts[index].openid, openid)) {
            continue;
        }
        if (removed != NULL) {
            *removed = s_contacts.contacts[index];
        }
        if (index + 1U < count) {
            memmove(&s_contacts.contacts[index],
                    &s_contacts.contacts[index + 1U],
                    sizeof(s_contacts.contacts[0]) * (count - index - 1U));
        }
        memset(&s_contacts.contacts[count - 1U],
               0,
               sizeof(s_contacts.contacts[count - 1U]));
        s_contacts.contact_count = count - 1U;
        if (str_same(s_contacts.cached_auth.openid, openid)) {
            memset(&s_contacts.cached_auth, 0, sizeof(s_contacts.cached_auth));
        }
        found = true;
        break;
    }
    xSemaphoreGive(s_contacts.lock);
    return found;
}

esp_err_t wechat_voip_contacts_update_remark(const char *openid,
                                             const char *remark,
                                             const char *source)
{
    if (openid == NULL || openid[0] == '\0' || !wechat_voip_remark_is_valid(remark)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = wechat_voip_contacts_init();
    if (ret != ESP_OK) {
        return ret;
    }

    bool found = false;
    bool changed = false;
    xSemaphoreTake(s_contacts.lock, portMAX_DELAY);
    uint8_t count = s_contacts.contact_count > WECHAT_VOIP_CONTACT_MAX ?
                    WECHAT_VOIP_CONTACT_MAX : s_contacts.contact_count;
    for (uint8_t index = 0; index < count; ++index) {
        if (!str_same(s_contacts.contacts[index].openid, openid)) {
            continue;
        }
        found = true;
        changed = !str_same(s_contacts.contacts[index].remark, remark);
        copy_remark(s_contacts.contacts[index].remark,
                    sizeof(s_contacts.contacts[index].remark),
                    remark);
        break;
    }
    if (str_same(s_contacts.cached_auth.openid, openid)) {
        found = true;
        changed = changed || !str_same(s_contacts.cached_auth.remark, remark);
        copy_remark(s_contacts.cached_auth.remark,
                    sizeof(s_contacts.cached_auth.remark),
                    remark);
    }
    xSemaphoreGive(s_contacts.lock);

    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }
    if (changed) {
        ESP_LOGD(TAG,
                 "runtime contact remark updated: source=%s",
                 source != NULL ? source : "server");
    }
    return ESP_OK;
}

void wechat_voip_contacts_find(const char *openid,
                               wechat_voip_auth_user_t *target)
{
    if (target == NULL) {
        return;
    }
    memset(target, 0, sizeof(*target));
    if (openid == NULL || openid[0] == '\0' ||
        wechat_voip_contacts_init() != ESP_OK) {
        return;
    }

    xSemaphoreTake(s_contacts.lock, portMAX_DELAY);
    uint8_t count = s_contacts.contact_count > WECHAT_VOIP_CONTACT_MAX ?
                    WECHAT_VOIP_CONTACT_MAX :
                    s_contacts.contact_count;
    for (uint8_t index = 0; index < count; ++index) {
        if (str_same(s_contacts.contacts[index].openid, openid)) {
            *target = s_contacts.contacts[index];
            break;
        }
    }
    if (target->openid[0] == '\0' &&
        str_same(s_contacts.cached_auth.openid, openid)) {
        *target = s_contacts.cached_auth;
    }
    xSemaphoreGive(s_contacts.lock);
}

void wechat_voip_contacts_get_snapshot(wechat_voip_contacts_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (wechat_voip_contacts_init() != ESP_OK) {
        return;
    }

    xSemaphoreTake(s_contacts.lock, portMAX_DELAY);
    snapshot->ready = s_contacts.ready;
    snapshot->server_synced = s_contacts.server_synced;
    snapshot->last_error = s_contacts.sync_error;
    uint8_t count = s_contacts.contact_count > WECHAT_VOIP_CONTACT_MAX ?
                    WECHAT_VOIP_CONTACT_MAX :
                    s_contacts.contact_count;
    for (uint8_t index = 0;
         index < count && snapshot->count < WECHAT_VOIP_CONTACT_MAX;
         ++index) {
        if (!contact_valid(&s_contacts.contacts[index])) {
            continue;
        }
        copy_str(snapshot->contacts[snapshot->count].open_id,
                 sizeof(snapshot->contacts[snapshot->count].open_id),
                 s_contacts.contacts[index].openid);
        copy_str(snapshot->contacts[snapshot->count].remark,
                 sizeof(snapshot->contacts[snapshot->count].remark),
                 s_contacts.contacts[index].remark);
        snapshot->count++;
    }
    xSemaphoreGive(s_contacts.lock);
}
