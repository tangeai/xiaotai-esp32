#include "app_ai_device_action.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app.h"
#include "device_call.h"
#include "device_online.h"
#include "network.h"
#include "wechat_voip_service.h"

#define APP_AI_MESSAGE_DYNAMIC_TEXT_MAX \
    ((int)(AI_CHAT_DEVICE_ACTION_CONTACT_NAME_MAX - 1U))

#define APP_AI_CONTACT_REFRESH_WAIT_MS 2500U
#define APP_AI_CONTACT_REFRESH_POLL_MS 50U

typedef enum {
    APP_AI_STATUS_FILTER_ONLINE = 0,
    APP_AI_STATUS_FILTER_OFFLINE,
    APP_AI_STATUS_FILTER_ALL,
} app_ai_status_filter_t;

typedef enum {
    APP_AI_CONTACT_SCOPE_AUTO = 0,
    APP_AI_CONTACT_SCOPE_DEVICE,
    APP_AI_CONTACT_SCOPE_WECHAT,
} app_ai_contact_scope_t;

typedef struct {
    ai_chat_device_action_route_t route;
    const device_call_contact_t *device;
    const wechat_voip_contact_t *wechat;
} app_ai_contact_match_t;

static char app_ai_ascii_lower(char ch)
{
    return ch >= 'A' && ch <= 'Z' ? (char)(ch - 'A' + 'a') : ch;
}

static bool app_ai_ascii_equal_ignore_case(const char *lhs, const char *rhs)
{
    if (lhs == NULL || rhs == NULL) {
        return false;
    }
    while (*lhs != '\0' && *rhs != '\0') {
        if (app_ai_ascii_lower(*lhs) != app_ai_ascii_lower(*rhs)) {
            return false;
        }
        ++lhs;
        ++rhs;
    }
    return *lhs == '\0' && *rhs == '\0';
}

static void app_ai_copy_trimmed(char *dst, size_t dst_size, const char *src)
{
    const char *begin = src;
    const char *end = NULL;

    if (dst == NULL || dst_size == 0U) {
        return;
    }
    dst[0] = '\0';
    if (src == NULL) {
        return;
    }

    while (*begin == ' ' || *begin == '\t' ||
           *begin == '\r' || *begin == '\n') {
        ++begin;
    }
    end = begin + strlen(begin);
    while (end > begin &&
           (end[-1] == ' ' || end[-1] == '\t' ||
            end[-1] == '\r' || end[-1] == '\n')) {
        --end;
    }

    size_t length = (size_t)(end - begin);
    if (length >= dst_size) {
        length = dst_size - 1U;
    }
    memcpy(dst, begin, length);
    dst[length] = '\0';
}

static void app_ai_set_result(ai_chat_device_action_result_t *result,
                              bool ok,
                              const char *status,
                              const char *message)
{
    if (result == NULL) {
        return;
    }
    result->ok = ok;
    strlcpy(result->status, status != NULL ? status : "", sizeof(result->status));
    strlcpy(result->message, message != NULL ? message : "", sizeof(result->message));
}

static bool app_ai_action_is_generic_call(const char *action)
{
    return app_ai_ascii_equal_ignore_case(action, "call_device") ||
           app_ai_ascii_equal_ignore_case(action, "device_call") ||
           app_ai_ascii_equal_ignore_case(action, "start_device_call") ||
           app_ai_ascii_equal_ignore_case(action, "call_contact") ||
           app_ai_ascii_equal_ignore_case(action, "call");
}

static bool app_ai_action_is_wechat_call(const char *action)
{
    return app_ai_ascii_equal_ignore_case(action, "call_wechat") ||
           app_ai_ascii_equal_ignore_case(action, "wechat_call") ||
           app_ai_ascii_equal_ignore_case(action, "call_wechat_contact") ||
           app_ai_ascii_equal_ignore_case(action, "call_voip_contact") ||
           app_ai_ascii_equal_ignore_case(action, "voip_call");
}

static bool app_ai_action_is_contact_status_query(const char *action)
{
    return app_ai_ascii_equal_ignore_case(action, "query_contact_status") ||
           app_ai_ascii_equal_ignore_case(action, "get_contact_status") ||
           app_ai_ascii_equal_ignore_case(action, "list_contact_status") ||
           app_ai_ascii_equal_ignore_case(action, "list_online_contacts") ||
           app_ai_ascii_equal_ignore_case(action, "query_online_contacts");
}

static bool app_ai_type_is_device(const char *type)
{
    return app_ai_ascii_equal_ignore_case(type, "device") ||
           app_ai_ascii_equal_ignore_case(type, "device_call") ||
           app_ai_ascii_equal_ignore_case(type, "tirtc") ||
           app_ai_ascii_equal_ignore_case(type, "设备") ||
           app_ai_ascii_equal_ignore_case(type, "设备联系人");
}

static bool app_ai_type_is_wechat(const char *type)
{
    return app_ai_ascii_equal_ignore_case(type, "wechat") ||
           app_ai_ascii_equal_ignore_case(type, "wechat_voip") ||
           app_ai_ascii_equal_ignore_case(type, "wx") ||
           app_ai_ascii_equal_ignore_case(type, "voip") ||
           app_ai_ascii_equal_ignore_case(type, "微信") ||
           app_ai_ascii_equal_ignore_case(type, "微信联系人");
}

static bool app_ai_call_type_is_audio(const char *call_type)
{
    return call_type == NULL ||
           call_type[0] == '\0' ||
           app_ai_ascii_equal_ignore_case(call_type, "audio") ||
           app_ai_ascii_equal_ignore_case(call_type, "voice") ||
           app_ai_type_is_device(call_type) ||
           app_ai_type_is_wechat(call_type);
}

static bool app_ai_call_type_is_video(const char *call_type)
{
    return call_type != NULL &&
           (app_ai_ascii_equal_ignore_case(call_type, "video") ||
            app_ai_ascii_equal_ignore_case(call_type, "video_call") ||
            app_ai_ascii_equal_ignore_case(call_type, "视频") ||
            app_ai_ascii_equal_ignore_case(call_type, "视频通话"));
}

bool app_ai_device_action_requests_video(const ai_chat_device_action_t *action)
{
    return action != NULL && app_ai_call_type_is_video(action->call_type);
}

static bool app_ai_device_contact_matches(const device_call_contact_t *contact,
                                          const char *target,
                                          bool exact)
{
    if (contact == NULL || target == NULL || target[0] == '\0') {
        return false;
    }
    if (app_ai_ascii_equal_ignore_case(contact->device_id, target)) {
        return true;
    }
    if (contact->remark[0] == '\0') {
        return false;
    }
    if (exact) {
        return strcmp(contact->remark, target) == 0;
    }
    return strstr(contact->remark, target) != NULL ||
           strstr(target, contact->remark) != NULL;
}

static bool app_ai_wechat_contact_matches(const wechat_voip_contact_t *contact,
                                          const char *target,
                                          bool exact)
{
    if (contact == NULL || target == NULL || target[0] == '\0') {
        return false;
    }
    if (strcmp(contact->open_id, target) == 0) {
        return true;
    }
    if (contact->remark[0] == '\0') {
        return false;
    }
    if (exact) {
        return strcmp(contact->remark, target) == 0;
    }
    return strstr(contact->remark, target) != NULL ||
           strstr(target, contact->remark) != NULL;
}

static const char *app_ai_device_contact_name(const device_call_contact_t *contact)
{
    if (contact == NULL) {
        return "";
    }
    return contact->remark[0] != '\0' ? contact->remark : contact->device_id;
}

static void app_ai_append_contact_result(ai_chat_device_action_result_t *result,
                                         const device_call_contact_t *contact)
{
    if (result == NULL || contact == NULL ||
        result->contact_count >= AI_CHAT_DEVICE_ACTION_CONTACT_MAX) {
        return;
    }

    ai_chat_device_action_contact_t *dst =
        &result->contacts[result->contact_count++];
    strlcpy(dst->name, app_ai_device_contact_name(contact), sizeof(dst->name));
    strlcpy(dst->device_id, contact->device_id, sizeof(dst->device_id));
    dst->online = contact->online;
}

static esp_err_t app_ai_refresh_device_contacts(
    device_call_contacts_snapshot_t *contacts,
    ai_chat_device_action_result_t *result)
{
    if (contacts == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!network_is_connected() || !device_online_is_online()) {
        app_ai_set_result(result, false, "network_offline", "设备当前未连接网络");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t refresh_ret = device_call_refresh_contacts_async();
    if (refresh_ret != ESP_OK) {
        app_ai_set_result(result,
                          false,
                          "contacts_unavailable",
                          "无法刷新联系人状态，请稍后再问");
        return refresh_ret;
    }

    uint32_t waited_ms = 0U;
    do {
        device_call_get_contacts_snapshot(contacts);
        if (!contacts->refreshing) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(APP_AI_CONTACT_REFRESH_POLL_MS));
        waited_ms += APP_AI_CONTACT_REFRESH_POLL_MS;
    } while (waited_ms < APP_AI_CONTACT_REFRESH_WAIT_MS);
    device_call_get_contacts_snapshot(contacts);

    if (contacts->refreshing) {
        app_ai_set_result(result,
                          false,
                          "contacts_loading",
                          "联系人状态正在刷新，请稍后再问");
        return ESP_ERR_TIMEOUT;
    }
    if (!contacts->ready) {
        app_ai_set_result(result,
                          false,
                          "contacts_loading",
                          "联系人状态尚未同步完成，请稍后再问");
        return ESP_ERR_INVALID_STATE;
    }
    if (contacts->last_error != ESP_OK) {
        app_ai_set_result(result,
                          false,
                          "contacts_unavailable",
                          "联系人状态同步失败，请稍后再问");
        return contacts->last_error;
    }
    return ESP_OK;
}

static esp_err_t app_ai_parse_status_filter(
    const char *value,
    app_ai_status_filter_t *filter,
    ai_chat_device_action_result_t *result)
{
    if (filter == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (value == NULL || value[0] == '\0' ||
        app_ai_ascii_equal_ignore_case(value, "online") ||
        app_ai_ascii_equal_ignore_case(value, "在线")) {
        *filter = APP_AI_STATUS_FILTER_ONLINE;
        return ESP_OK;
    }
    if (app_ai_ascii_equal_ignore_case(value, "offline") ||
        app_ai_ascii_equal_ignore_case(value, "离线")) {
        *filter = APP_AI_STATUS_FILTER_OFFLINE;
        return ESP_OK;
    }
    if (app_ai_ascii_equal_ignore_case(value, "all") ||
        app_ai_ascii_equal_ignore_case(value, "any") ||
        app_ai_ascii_equal_ignore_case(value, "*") ||
        app_ai_ascii_equal_ignore_case(value, "全部") ||
        app_ai_ascii_equal_ignore_case(value, "所有")) {
        *filter = APP_AI_STATUS_FILTER_ALL;
        return ESP_OK;
    }

    app_ai_set_result(result,
                      false,
                      "unsupported_status_filter",
                      "状态筛选只支持 online、offline 或 all");
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t app_ai_query_contact_status(
    const ai_chat_device_action_t *action,
    const char *target,
    ai_chat_device_action_result_t *result)
{
    device_call_contacts_snapshot_t contacts = {0};
    app_ai_status_filter_t filter = APP_AI_STATUS_FILTER_ONLINE;
    const device_call_contact_t *selected = NULL;
    uint8_t match_count = 0U;
    uint8_t online_count = 0U;
    uint8_t offline_count = 0U;
    char message[AI_CHAT_DEVICE_ACTION_MESSAGE_MAX] = {0};

    if (action->contact_type[0] != '\0' &&
        !app_ai_type_is_device(action->contact_type)) {
        bool wechat = app_ai_type_is_wechat(action->contact_type);
        app_ai_set_result(result,
                          false,
                          wechat ? "wechat_status_unsupported" :
                                   "unsupported_contact_type",
                          wechat ? "平台不提供微信联系人的在线状态" :
                                   "在线状态查询只支持设备联系人");
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t ret =
        app_ai_parse_status_filter(action->status_filter, &filter, result);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = app_ai_refresh_device_contacts(&contacts, result);
    if (ret != ESP_OK) {
        return ret;
    }

    result->has_contacts_result = true;
    if (target != NULL && target[0] != '\0') {
        for (int pass = 0; pass < 2 && match_count == 0U; ++pass) {
            for (uint8_t index = 0U; index < contacts.count; ++index) {
                const device_call_contact_t *contact = &contacts.contacts[index];
                if (contact->device_id[0] == '\0' ||
                    !app_ai_device_contact_matches(contact,
                                                   target,
                                                   pass == 0)) {
                    continue;
                }
                ++match_count;
                if (selected == NULL) {
                    selected = contact;
                }
            }
        }

        if (match_count == 0U || selected == NULL) {
            snprintf(message,
                     sizeof(message),
                     "没有找到名为%.*s的设备联系人",
                     APP_AI_MESSAGE_DYNAMIC_TEXT_MAX,
                     target);
            app_ai_set_result(result, false, "not_found", message);
            result->has_contacts_result = false;
            return ESP_ERR_NOT_FOUND;
        }
        if (match_count > 1U) {
            app_ai_set_result(result,
                              false,
                              "ambiguous",
                              "匹配到多个设备联系人，请说得更具体一点");
            result->has_contacts_result = false;
            return ESP_ERR_INVALID_STATE;
        }

        app_ai_append_contact_result(result, selected);
        snprintf(message,
                 sizeof(message),
                 "%.*s当前%s",
                 APP_AI_MESSAGE_DYNAMIC_TEXT_MAX,
                 app_ai_device_contact_name(selected),
                 selected->online ? "在线" : "离线");
        app_ai_set_result(result, true, "ok", message);
        return ESP_OK;
    }

    for (uint8_t index = 0U; index < contacts.count; ++index) {
        const device_call_contact_t *contact = &contacts.contacts[index];
        if (contact->device_id[0] == '\0') {
            continue;
        }
        if (contact->online) {
            ++online_count;
        } else {
            ++offline_count;
        }
        if ((filter == APP_AI_STATUS_FILTER_ONLINE && !contact->online) ||
            (filter == APP_AI_STATUS_FILTER_OFFLINE && contact->online)) {
            continue;
        }
        app_ai_append_contact_result(result, contact);
    }

    if (filter == APP_AI_STATUS_FILTER_OFFLINE) {
        snprintf(message, sizeof(message), "当前有%u个设备联系人离线",
                 (unsigned)offline_count);
    } else if (filter == APP_AI_STATUS_FILTER_ALL) {
        snprintf(message,
                 sizeof(message),
                 "共有%u个设备联系人，%u个在线，%u个离线",
                 (unsigned)(online_count + offline_count),
                 (unsigned)online_count,
                 (unsigned)offline_count);
    } else {
        snprintf(message, sizeof(message), "当前有%u个设备联系人在线",
                 (unsigned)online_count);
    }
    app_ai_set_result(result, true, "ok", message);
    return ESP_OK;
}

static esp_err_t app_ai_pick_contact_scope(
    const ai_chat_device_action_t *action,
    app_ai_contact_scope_t *scope,
    ai_chat_device_action_result_t *result)
{
    const char *type = action->contact_type;

    if (type[0] == '\0' &&
        (app_ai_type_is_device(action->call_type) ||
         app_ai_type_is_wechat(action->call_type))) {
        type = action->call_type;
    }
    if (type[0] != '\0') {
        if (app_ai_type_is_device(type)) {
            *scope = APP_AI_CONTACT_SCOPE_DEVICE;
            return ESP_OK;
        }
        if (app_ai_type_is_wechat(type)) {
            *scope = APP_AI_CONTACT_SCOPE_WECHAT;
            return ESP_OK;
        }
        app_ai_set_result(result,
                          false,
                          "unsupported_contact_type",
                          "联系人类型只支持设备联系人或微信联系人");
        return ESP_ERR_NOT_SUPPORTED;
    }

    *scope = app_ai_action_is_wechat_call(action->action) ?
                 APP_AI_CONTACT_SCOPE_WECHAT :
                 APP_AI_CONTACT_SCOPE_AUTO;
    return ESP_OK;
}

static void app_ai_count_contact_matches(
    const device_call_contacts_snapshot_t *device_contacts,
    const wechat_voip_contacts_snapshot_t *wechat_contacts,
    app_ai_contact_scope_t scope,
    const char *target,
    bool exact,
    app_ai_contact_match_t *selected,
    uint8_t *match_count)
{
    if (scope != APP_AI_CONTACT_SCOPE_WECHAT && device_contacts->ready) {
        for (uint8_t index = 0U; index < device_contacts->count; ++index) {
            const device_call_contact_t *contact =
                &device_contacts->contacts[index];
            if (strlen(contact->device_id) != APP_CALL_CONTACT_DEVICE_ID_LENGTH ||
                !app_ai_device_contact_matches(contact, target, exact)) {
                continue;
            }
            ++(*match_count);
            if (selected->route == AI_CHAT_DEVICE_ACTION_ROUTE_NONE) {
                selected->route = AI_CHAT_DEVICE_ACTION_ROUTE_DEVICE_CALL;
                selected->device = contact;
            }
        }
    }

    if (scope != APP_AI_CONTACT_SCOPE_DEVICE && wechat_contacts->ready) {
        for (uint8_t index = 0U; index < wechat_contacts->count; ++index) {
            const wechat_voip_contact_t *contact =
                &wechat_contacts->contacts[index];
            if (!app_ai_wechat_contact_matches(contact, target, exact)) {
                continue;
            }
            ++(*match_count);
            if (selected->route == AI_CHAT_DEVICE_ACTION_ROUTE_NONE) {
                selected->route = AI_CHAT_DEVICE_ACTION_ROUTE_WECHAT_VOIP;
                selected->wechat = contact;
            }
        }
    }
}

static esp_err_t app_ai_resolve_call_target(
    app_ai_contact_scope_t scope,
    const char *target,
    ai_chat_device_action_result_t *result)
{
    device_call_contacts_snapshot_t device_contacts = {0};
    wechat_voip_contacts_snapshot_t wechat_contacts = {0};
    app_ai_contact_match_t selected = {0};
    uint8_t match_count = 0U;
    char message[AI_CHAT_DEVICE_ACTION_MESSAGE_MAX] = {0};

    if (scope != APP_AI_CONTACT_SCOPE_WECHAT) {
        device_call_get_contacts_snapshot(&device_contacts);
        if (!device_contacts.ready &&
            !device_contacts.refreshing &&
            device_online_is_online()) {
            (void)device_call_refresh_contacts_async();
        }
    }
    if (scope != APP_AI_CONTACT_SCOPE_DEVICE) {
        wechat_voip_service_get_contacts(&wechat_contacts);
        if (!wechat_contacts.server_synced) {
            (void)wechat_voip_service_refresh_contacts_async();
        }
    }

    for (int pass = 0; pass < 2 && match_count == 0U; ++pass) {
        app_ai_count_contact_matches(&device_contacts,
                                     &wechat_contacts,
                                     scope,
                                     target,
                                     pass == 0,
                                     &selected,
                                     &match_count);
    }

    if (match_count == 0U) {
        bool device_loading =
            scope != APP_AI_CONTACT_SCOPE_WECHAT && !device_contacts.ready;
        bool wechat_loading =
            scope != APP_AI_CONTACT_SCOPE_DEVICE &&
            (!wechat_contacts.ready || !wechat_contacts.server_synced);
        if (device_loading || wechat_loading) {
            app_ai_set_result(result,
                              false,
                              "contacts_loading",
                              "联系人列表正在同步，请稍后再试");
            return ESP_ERR_INVALID_STATE;
        }

        bool contacts_empty =
            (scope == APP_AI_CONTACT_SCOPE_DEVICE &&
             device_contacts.count == 0U) ||
            (scope == APP_AI_CONTACT_SCOPE_WECHAT &&
             wechat_contacts.count == 0U) ||
            (scope == APP_AI_CONTACT_SCOPE_AUTO &&
             device_contacts.count == 0U &&
             wechat_contacts.count == 0U);
        if (contacts_empty) {
            app_ai_set_result(result,
                              false,
                              "contacts_empty",
                              scope == APP_AI_CONTACT_SCOPE_WECHAT ?
                                  "当前没有已授权的微信联系人" :
                                  "当前没有可呼叫的联系人");
            return ESP_ERR_NOT_FOUND;
        }

        snprintf(message,
                 sizeof(message),
                 "没有找到名为%.*s的%s",
                 APP_AI_MESSAGE_DYNAMIC_TEXT_MAX,
                 target,
                 scope == APP_AI_CONTACT_SCOPE_WECHAT ?
                     "微信联系人" : "联系人");
        app_ai_set_result(result, false, "not_found", message);
        return ESP_ERR_NOT_FOUND;
    }
    if (match_count > 1U) {
        app_ai_set_result(result,
                          false,
                          "ambiguous",
                          "匹配到多个联系人，请说得更具体一点");
        return ESP_ERR_INVALID_STATE;
    }
    if (selected.route == AI_CHAT_DEVICE_ACTION_ROUTE_DEVICE_CALL &&
        (selected.device == NULL || !selected.device->online)) {
        snprintf(message,
                 sizeof(message),
                 "%.*s当前不在线",
                 APP_AI_MESSAGE_DYNAMIC_TEXT_MAX,
                 selected.device != NULL &&
                         selected.device->remark[0] != '\0' ?
                     selected.device->remark : target);
        app_ai_set_result(result, false, "offline", message);
        return ESP_ERR_INVALID_STATE;
    }
    if (selected.route == AI_CHAT_DEVICE_ACTION_ROUTE_WECHAT_VOIP &&
        !wechat_voip_service_is_connected()) {
        app_ai_set_result(result,
                          false,
                          "service_loading",
                          "微信呼叫服务正在连接，请稍后再试");
        return ESP_ERR_INVALID_STATE;
    }

    result->ok = true;
    result->call_route = selected.route;
    strlcpy(result->status, "accepted", sizeof(result->status));
    if (selected.route == AI_CHAT_DEVICE_ACTION_ROUTE_DEVICE_CALL &&
        selected.device != NULL) {
        strlcpy(result->target_id,
                selected.device->device_id,
                sizeof(result->target_id));
        strlcpy(result->matched_name,
                selected.device->remark[0] != '\0' ?
                    selected.device->remark : selected.device->device_id,
                sizeof(result->matched_name));
    } else if (selected.route == AI_CHAT_DEVICE_ACTION_ROUTE_WECHAT_VOIP &&
               selected.wechat != NULL) {
        strlcpy(result->target_id,
                selected.wechat->open_id,
                sizeof(result->target_id));
        strlcpy(result->matched_name,
                selected.wechat->remark[0] != '\0' ?
                    selected.wechat->remark : "微信联系人",
                sizeof(result->matched_name));
    } else {
        app_ai_set_result(result, false, "internal_error", "联系人解析结果无效");
        result->call_route = AI_CHAT_DEVICE_ACTION_ROUTE_NONE;
        return ESP_FAIL;
    }

    strlcpy(result->message, "已受理呼叫", sizeof(result->message));
    strlcat(result->message, result->matched_name, sizeof(result->message));
    return ESP_OK;
}

static bool app_ai_other_call_is_busy(void)
{
    device_call_snapshot_t call = {0};
    device_call_get_snapshot(&call);

    if (call.pending_incoming ||
        call.state == DEVICE_CALL_STATE_OUTGOING ||
        call.state == DEVICE_CALL_STATE_INCOMING ||
        call.state == DEVICE_CALL_STATE_CONNECTING ||
        call.state == DEVICE_CALL_STATE_IN_CALL) {
        return true;
    }
    return wechat_voip_service_get_call_state() !=
           WECHAT_VOIP_CALL_STATE_IDLE;
}

esp_err_t app_ai_device_action_execute(const ai_chat_device_action_t *action,
                                       ai_chat_device_action_result_t *result)
{
    char target[AI_CHAT_DEVICE_ACTION_TARGET_MAX] = {0};
    app_ai_contact_scope_t scope = APP_AI_CONTACT_SCOPE_AUTO;

    if (action == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));

    app_ai_copy_trimmed(target, sizeof(target), action->target);
    if (app_ai_action_is_contact_status_query(action->action)) {
        return app_ai_query_contact_status(action, target, result);
    }
    if (!app_ai_action_is_generic_call(action->action) &&
        !app_ai_action_is_wechat_call(action->action)) {
        app_ai_set_result(result,
                          false,
                          "unsupported",
                          "当前只支持呼叫联系人或查询设备联系人状态");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!app_ai_call_type_is_audio(action->call_type) &&
        !app_ai_call_type_is_video(action->call_type)) {
        app_ai_set_result(result,
                          false,
                          "unsupported_call_type",
                          "当前只支持语音或视频呼叫");
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t scope_ret = app_ai_pick_contact_scope(action, &scope, result);
    if (scope_ret != ESP_OK) {
        return scope_ret;
    }
    if (!wechat_voip_service_is_enabled()) {
        if (scope == APP_AI_CONTACT_SCOPE_WECHAT) {
            app_ai_set_result(result,
                              false,
                              "unsupported",
                              "当前固件未启用微信呼叫");
            return ESP_ERR_NOT_SUPPORTED;
        }
        scope = APP_AI_CONTACT_SCOPE_DEVICE;
    }
    if (!network_is_connected()) {
        app_ai_set_result(result, false, "network_offline", "设备当前未连接网络");
        return ESP_ERR_INVALID_STATE;
    }
    if (app_ai_other_call_is_busy()) {
        app_ai_set_result(result, false, "busy", "设备当前正在通话");
        return ESP_ERR_INVALID_STATE;
    }
    if (target[0] == '\0') {
        app_ai_set_result(result,
                          false,
                          "missing_target",
                          "请告诉我要呼叫哪个联系人");
        return ESP_ERR_INVALID_ARG;
    }

    return app_ai_resolve_call_target(scope, target, result);
}
