#include "wechat_voip_api.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "device_auth_http.h"
#include "esp_check.h"
#include "esp_log.h"
#include "thing_http_client.h"
#include "wechat_voip_config.h"

static const char *TAG = "wx_voip_api";

_Static_assert(WECHAT_VOIP_SCREEN_WIDTH == 640 &&
                   WECHAT_VOIP_SCREEN_HEIGHT == 480,
               "P4 WeChat VoIP profile must request the standard landscape rendition");
_Static_assert(WECHAT_VOIP_CAMERA_ROTATION == 0 ||
                   WECHAT_VOIP_CAMERA_ROTATION == 90 ||
                   WECHAT_VOIP_CAMERA_ROTATION == 180 ||
                   WECHAT_VOIP_CAMERA_ROTATION == 270,
               "WeChat VoIP camera rotation must be 0, 90, 180, or 270 degrees");

enum {
    VOIP_HTTP_URL_MAX_LEN = 256,
    VOIP_HTTP_RESPONSE_MAX_LEN = 2048,
    VOIP_HTTP_BODY_MAX_LEN = 768,
    VOIP_PROFILE_MAX_LEN = 512,
    DEVICE_AUDIO_RATE = 8000,
    DEVICE_AUDIO_CHANNELS = 1,
    DEVICE_CALLING_TIMEOUT_SEC = 30,
};

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

static bool response_code_ok(cJSON *root)
{
    cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    int code_value = cJSON_IsNumber(code) ? code->valueint : -1;
    return code_value == 0 || code_value == 200;
}

static const char *json_string_any(cJSON *root, const char *name1, const char *name2)
{
    if (root == NULL || name1 == NULL) {
        return NULL;
    }
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, name1));
    if ((value == NULL || value[0] == '\0') && name2 != NULL) {
        value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, name2));
    }
    return value;
}

static const char *json_string_any4(cJSON *root,
                                    const char *name1,
                                    const char *name2,
                                    const char *name3,
                                    const char *name4)
{
    const char *names[] = {name1, name2, name3, name4};
    if (root == NULL) {
        return NULL;
    }
    for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, names[index]));
        if (value != NULL && value[0] != '\0') {
            return value;
        }
    }
    return NULL;
}

static esp_err_t voip_http_request(const char *api_base,
                                   const char *path,
                                   const char *method,
                                   const char *body,
                                   const char *mqtt_token,
                                   char *response,
                                   size_t response_size,
                                   int *status)
{
    char url[VOIP_HTTP_URL_MAX_LEN] = {0};
    ESP_RETURN_ON_ERROR(thing_http_join_url(url, sizeof(url), api_base, path),
                        TAG,
                        "voip url build failed");

    char authorization[DEVICE_AUTH_MQTT_TOKEN_MAX_LEN + 16] = {0};
    thing_http_header_t headers[1];
    size_t header_count = 0;
    if (mqtt_token != NULL && mqtt_token[0] != '\0') {
        snprintf(authorization, sizeof(authorization), "Bearer %s", mqtt_token);
        headers[0].name = "Authorization";
        headers[0].value = authorization;
        header_count = 1;
    }

    const thing_http_request_t request = {
        .url = url,
        .method = method,
        .body = body,
        .headers = headers,
        .header_count = header_count,
    };
    return thing_http_request_json(&request, response, response_size, status);
}

static esp_err_t parse_and_check_reply(const char *response, const char *operation)
{
    cJSON *root = cJSON_Parse(response);
    if (root == NULL) {
        ESP_LOGW(TAG, "%s response is not JSON: %.120s", operation, response != NULL ? response : "");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!response_code_ok(root)) {
        cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "msg");
        ESP_LOGW(TAG, "%s rejected: code=%d msg=%s",
                 operation,
                 cJSON_IsNumber(code) ? code->valueint : -1,
                 cJSON_GetStringValue(msg) != NULL ? cJSON_GetStringValue(msg) : "");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t wechat_voip_api_report_profile(const char *api_base, const char *mqtt_token)
{
    char response[VOIP_HTTP_RESPONSE_MAX_LEN] = {0};
    int status = 0;
    char body[VOIP_PROFILE_MAX_LEN + 1] = {0};
    const char *object_fit = WECHAT_VOIP_OBJECT_FIT;
    const bool local_video = WECHAT_VOIP_LOCAL_VIDEO_ENABLE != 0;
    const bool remote_video = WECHAT_VOIP_REMOTE_VIDEO_ENABLE != 0;
    const bool any_video = local_video || remote_video;
    cJSON *profile = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(profile != NULL, ESP_ERR_NO_MEM, TAG, "create voip profile failed");

    if (strcmp(object_fit, "fill") != 0 && strcmp(object_fit, "contain") != 0) {
        ESP_LOGW(TAG,
                 "invalid profile object_fit=%s; fallback=contain",
                 object_fit);
        object_fit = "contain";
    }

    /*
     * Request the standard 640x480 landscape rendition. The physical 480x320
     * panel contract remains inside the renderer, and adaptive service output
     * may still temporarily arrive at a smaller size.
     * Rotation, mirror, aspect_ratio, and object_fit are extension hints.
     * P4 publishes hardware-encoded H264, while the service converts WeChat
     * downlink to independent MJPEG frames for the hardware JPEG decoder.
     */
    cJSON_AddNumberToObject(profile,
                           "screen_width",
                           WECHAT_VOIP_SCREEN_WIDTH);
    cJSON_AddNumberToObject(profile,
                           "screen_height",
                           WECHAT_VOIP_SCREEN_HEIGHT);
    cJSON_AddNumberToObject(profile, "camera_rotation", WECHAT_VOIP_CAMERA_ROTATION);
    cJSON_AddNumberToObject(profile,
                           "aspect_ratio",
                           (double)WECHAT_VOIP_VIDEO_WIDTH /
                               (double)WECHAT_VOIP_VIDEO_HEIGHT);
    cJSON_AddBoolToObject(profile, "hor_mirror", false);
    cJSON_AddBoolToObject(profile, "vert_mirror", false);
    cJSON_AddStringToObject(profile, "object_fit", object_fit);
    cJSON_AddNumberToObject(profile, "audio_rate", DEVICE_AUDIO_RATE);
    cJSON_AddNumberToObject(profile, "audio_channels", DEVICE_AUDIO_CHANNELS);
    cJSON_AddStringToObject(profile,
                           "up_video_mt",
                           local_video ? WECHAT_VOIP_UP_VIDEO_MEDIA : "none");
    cJSON_AddStringToObject(profile,
                           "down_video_mt",
                           remote_video ? WECHAT_VOIP_DOWN_VIDEO_MEDIA : "none");
    cJSON_AddStringToObject(profile, "down_audio_mt", "alaw");
    cJSON_AddBoolToObject(profile, "no_video", !any_video);
    cJSON_AddNumberToObject(profile, "calling_timeout_sec", DEVICE_CALLING_TIMEOUT_SEC);

    bool printed = cJSON_PrintPreallocated(profile, body, sizeof(body), false);
    cJSON_Delete(profile);
    ESP_RETURN_ON_FALSE(printed && strlen(body) <= VOIP_PROFILE_MAX_LEN,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "voip profile exceeds %u bytes",
                        (unsigned)VOIP_PROFILE_MAX_LEN);

    ESP_LOGI(TAG,
             "report voip profile: request_down=%s-%ux%u "
             "server_fit=%s "
             "camera_rotation=%u request_up=%s-%ux%u "
             "audio=%uHz/%uch bytes=%u",
             remote_video ? WECHAT_VOIP_DOWN_VIDEO_MEDIA : "none",
             (unsigned)WECHAT_VOIP_SCREEN_WIDTH,
             (unsigned)WECHAT_VOIP_SCREEN_HEIGHT,
             object_fit,
             (unsigned)WECHAT_VOIP_CAMERA_ROTATION,
             local_video ? WECHAT_VOIP_UP_VIDEO_MEDIA : "none",
             (unsigned)WECHAT_VOIP_VIDEO_WIDTH,
             (unsigned)WECHAT_VOIP_VIDEO_HEIGHT,
             (unsigned)DEVICE_AUDIO_RATE,
             (unsigned)DEVICE_AUDIO_CHANNELS,
             (unsigned)strlen(body));
    esp_err_t ret = voip_http_request(api_base,
                                      "/v1/voip/device/profile",
                                      "POST",
                                      body,
                                      mqtt_token,
                                      response,
                                      sizeof(response),
                                      &status);
    if (ret != ESP_OK) {
        return ret;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "report profile HTTP status=%d body_len=%u",
                 status,
                 (unsigned)strlen(response));
        return ESP_FAIL;
    }
    return parse_and_check_reply(response, "report profile");
}

esp_err_t wechat_voip_api_fetch_callers(const char *api_base,
                                        const char *mqtt_token,
                                        wechat_voip_api_caller_cb_t caller_cb,
                                        void *ctx,
                                        int *caller_count)
{
    char response[VOIP_HTTP_RESPONSE_MAX_LEN] = {0};
    int status = 0;
    const char *path = "/v1/voip/device/contacts";

    esp_err_t ret = voip_http_request(api_base,
                                      path,
                                      "GET",
                                      NULL,
                                      mqtt_token,
                                      response,
                                      sizeof(response),
                                      &status);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "fetch callers failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (status == 404 || status == 405) {
        /* Keep interoperability with deployments that still expose the
         * pre-contacts compatibility endpoint. */
        path = "/v1/voip/device/callers";
        memset(response, 0, sizeof(response));
        ret = voip_http_request(api_base,
                                path,
                                "GET",
                                NULL,
                                mqtt_token,
                                response,
                                sizeof(response),
                                &status);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "fetch callers compatibility request failed: %s",
                     esp_err_to_name(ret));
            return ret;
        }
    }
    if (status != 200) {
        ESP_LOGW(TAG, "fetch callers HTTP status=%d path=%s body_len=%u",
                 status,
                 path,
                 (unsigned)strlen(response));
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(response);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!response_code_ok(root)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *list = cJSON_IsObject(data) ?
        cJSON_GetObjectItemCaseSensitive(data, "contacts") : NULL;
    if (!cJSON_IsArray(list) && cJSON_IsObject(data)) {
        list = cJSON_GetObjectItemCaseSensitive(data, "list");
    }
    if (!cJSON_IsArray(list)) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "fetch callers response missing list");
        return ESP_ERR_INVALID_RESPONSE;
    }

    int count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, list) {
        wechat_voip_auth_user_t caller = {0};
        copy_str(caller.openid,
                 sizeof(caller.openid),
                 json_string_any4(item,
                                  "wx_open_id",
                                  "wxa_open_id",
                                  "wx_user_openid",
                                  "wxa_user_openid"));
        copy_str(caller.model_id, sizeof(caller.model_id), json_string_any(item, "wx_model_id", "wxa_model_id"));
        copy_str(caller.app_id, sizeof(caller.app_id), json_string_any(item, "wx_app_id", "wxa_app_id"));
        const char *remark =
            json_string_any4(item, "remark", "alias", "contact_name", "nickname");
        if (wechat_voip_remark_is_valid(remark != NULL ? remark : "")) {
            copy_str(caller.remark, sizeof(caller.remark), remark != NULL ? remark : "");
        }
        if (caller_cb != NULL) {
            caller_cb(&caller, ctx);
        }
        count++;
    }
    cJSON_Delete(root);
    if (caller_count != NULL) {
        *caller_count = count;
    }
    ESP_LOGD(TAG, "callers response parsed: count=%d", count);
    return ESP_OK;
}

esp_err_t wechat_voip_api_request_call(const char *api_base,
                                       const char *mqtt_token,
                                       const char *device_id,
                                       const wechat_voip_auth_user_t *target,
                                       wechat_voip_call_media_t call_media,
                                       int wx_version_type)
{
    if (device_id == NULL || device_id[0] == '\0' ||
        target == NULL || target->openid[0] == '\0' || target->model_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (call_media != WECHAT_VOIP_CALL_MEDIA_AUDIO &&
        call_media != WECHAT_VOIP_CALL_MEDIA_VIDEO) {
        return ESP_ERR_INVALID_ARG;
    }

    const bool video_call = call_media == WECHAT_VOIP_CALL_MEDIA_VIDEO;
    if (video_call &&
        !WECHAT_VOIP_LOCAL_VIDEO_ENABLE &&
        !WECHAT_VOIP_REMOTE_VIDEO_ENABLE) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    char body[VOIP_HTTP_BODY_MAX_LEN];
    /* Platform camera status uses 0 for enabled and 1 for disabled. */
    const char *room_type = video_call ? "video" : "voice";
    const bool local_video = video_call && WECHAT_VOIP_LOCAL_VIDEO_ENABLE;
    const bool remote_video = video_call && WECHAT_VOIP_REMOTE_VIDEO_ENABLE;
    const unsigned local_camera_status = local_video ? 0U : 1U;
    const unsigned remote_camera_status = remote_video ? 0U : 1U;
    int written = 0;
    if (target->app_id[0] != '\0') {
        written = snprintf(body,
                           sizeof(body),
                           "{\"device_id\":\"%s\","
                           "\"wx_app_id\":\"%s\","
                           "\"wx_user_openid\":\"%s\","
                           "\"wx_model_id\":\"%s\","
                           "\"wx_room_type\":\"%s\","
                           "\"wx_version_type\":%d,"
                           "\"calling_timeout_sec\":%u,"
                           "\"wx_caller_camera_status\":%u,"
                           "\"wx_listener_camera_status\":%u}",
                           device_id,
                           target->app_id,
                           target->openid,
                           target->model_id,
                           room_type,
                           wx_version_type,
                           (unsigned)DEVICE_CALLING_TIMEOUT_SEC,
                           local_camera_status,
                           remote_camera_status);
    } else {
        written = snprintf(body,
                           sizeof(body),
                           "{\"device_id\":\"%s\","
                           "\"wx_user_openid\":\"%s\","
                           "\"wx_model_id\":\"%s\","
                           "\"wx_room_type\":\"%s\","
                           "\"wx_version_type\":%d,"
                           "\"calling_timeout_sec\":%u,"
                           "\"wx_caller_camera_status\":%u,"
                           "\"wx_listener_camera_status\":%u}",
                           device_id,
                           target->openid,
                           target->model_id,
                           room_type,
                           wx_version_type,
                           (unsigned)DEVICE_CALLING_TIMEOUT_SEC,
                           local_camera_status,
                           remote_camera_status);
    }
    if (written <= 0 || written >= (int)sizeof(body)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char response[VOIP_HTTP_RESPONSE_MAX_LEN] = {0};
    int status = 0;
    ESP_LOGI(TAG,
             "request wechat call: device_id=%s room=%s local_camera=%s(status=%u) "
             "remote_camera=%s(status=%u) openid_len=%u model_id_len=%u "
             "version_type=%d calling_timeout=%us "
             "down_profile=%s/%ux%u",
             device_id,
             room_type,
             local_video ? "on" : "off",
             local_camera_status,
             remote_video ? "on" : "off",
             remote_camera_status,
             (unsigned)strlen(target->openid),
             (unsigned)strlen(target->model_id),
             wx_version_type,
             (unsigned)DEVICE_CALLING_TIMEOUT_SEC,
             remote_video ?
                  WECHAT_VOIP_DOWN_VIDEO_MEDIA :
                  "none",
             (unsigned)WECHAT_VOIP_SCREEN_WIDTH,
             (unsigned)WECHAT_VOIP_SCREEN_HEIGHT);
    esp_err_t ret = voip_http_request(api_base,
                                      "/v1/voip/device/call",
                                      "POST",
                                      body,
                                      mqtt_token,
                                      response,
                                      sizeof(response),
                                      &status);
    if (ret != ESP_OK) {
        return ret;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "device call HTTP status=%d body_len=%u",
                 status,
                 (unsigned)strlen(response));
        return ESP_FAIL;
    }
    esp_err_t parse_ret = parse_and_check_reply(response, "device call");
    if (parse_ret != ESP_OK) {
        return parse_ret;
    }

    const char *call_id = "";
    cJSON *root = cJSON_Parse(response);
    if (root != NULL) {
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        const cJSON *call_id_item = cJSON_IsObject(data) ?
                                        cJSON_GetObjectItemCaseSensitive(data, "call_id") :
                                        NULL;
        const char *value = cJSON_GetStringValue(call_id_item);
        if (value != NULL) {
            call_id = value;
        }
    }
    ESP_LOGI(TAG,
             "wechat call accepted: device_id=%s call_id=%s",
             device_id,
             call_id[0] != '\0' ? call_id : "-");
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t wechat_voip_api_update_contact_remark(const char *api_base,
                                                const char *mqtt_token,
                                                const char *peer_id,
                                                const char *remark)
{
    if (api_base == NULL || api_base[0] == '\0' ||
        mqtt_token == NULL || mqtt_token[0] == '\0' ||
        peer_id == NULL || peer_id[0] == '\0' ||
        !wechat_voip_remark_is_valid(remark)) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL ||
        cJSON_AddStringToObject(payload, "peer_id", peer_id) == NULL ||
        cJSON_AddStringToObject(payload, "remark", remark) == NULL) {
        cJSON_Delete(payload);
        return ESP_ERR_NO_MEM;
    }

    char body[VOIP_HTTP_BODY_MAX_LEN] = {0};
    bool encoded = cJSON_PrintPreallocated(payload, body, sizeof(body), false);
    cJSON_Delete(payload);
    if (!encoded) {
        return ESP_ERR_INVALID_SIZE;
    }

    char response[VOIP_HTTP_RESPONSE_MAX_LEN] = {0};
    int status = 0;
    esp_err_t ret = voip_http_request(api_base,
                                      "/v1/call/device/contacts/remark",
                                      "PUT",
                                      body,
                                      mqtt_token,
                                      response,
                                      sizeof(response),
                                      &status);
    if (ret != ESP_OK) {
        return ret;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG,
                 "update contact remark HTTP status=%d body_len=%u",
                 status,
                 (unsigned)strlen(response));
        return ESP_FAIL;
    }
    return parse_and_check_reply(response, "update contact remark");
}
