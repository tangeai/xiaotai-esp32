/* UART maintenance controls use the same public intents as the product UI. */
#include "regression_controls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "coprocessor_update.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "starter_media.h"
#include "starter_runtime.h"
#include "starter_product.h"
#include "starter_tirtc.h"
#include "wifi_manager.h"

static void result(esp_err_t err)
{
    flockfile(stdout);
    if (err == ESP_OK) puts("OK");
    else printf("ERROR,INTENT,%s\r\n", esp_err_to_name(err));
    fflush(stdout);
    funlockfile(stdout);
}

static bool contact_index(const char *value, uint8_t *index)
{
    if (!value || value[0] < '0' || value[0] > '9' || value[1] ||
        (unsigned)(value[0] - '0') >= STARTER_PRODUCT_CONTACTS_MAX) return false;
    *index = (uint8_t)(value[0] - '0');
    return true;
}

static bool idle(void)
{
    return starter_runtime_status().state == STARTER_RUNTIME_WAITING &&
           !starter_media_status().active && !starter_tirtc_connected() &&
           !coprocessor_update_busy();
}

static void product_snapshot(bool contacts)
{
    starter_runtime_product_snapshot_t *snapshot = heap_caps_calloc(
        1, sizeof(*snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snapshot) { result(ESP_ERR_NO_MEM); return; }
    if (!starter_runtime_try_product_snapshot(snapshot)) {
        heap_caps_free(snapshot);
        result(ESP_ERR_TIMEOUT);
        return;
    }
    const starter_runtime_status_t runtime = starter_runtime_status();
    flockfile(stdout);
    printf("+CALL:state=%s,session=%lu,connection=%lu,incoming=%u,wechat=%u,muted=%u,video=%u,camera=%u,contacts=%u\r\n",
           starter_runtime_state_name(runtime.state), (unsigned long)runtime.session_generation,
           (unsigned long)runtime.connection_generation, (unsigned)snapshot->call_incoming,
           (unsigned)snapshot->call_wechat, (unsigned)snapshot->call_microphone_muted,
           (unsigned)snapshot->call_video, (unsigned)snapshot->call_camera_enabled,
           (unsigned)snapshot->contact_count);
    if (contacts) for (unsigned i = 0; i < snapshot->contact_count; ++i) {
        const starter_product_contact_t *contact = &snapshot->contacts[i];
        printf("+CONTACT:index=%u,source=%s,online=%u,id=%s\r\n", i,
               contact->source == STARTER_CONTACT_DEVICE ? "device" : "wechat",
               (unsigned)contact->online,
               contact->source == STARTER_CONTACT_DEVICE ? contact->id : "[wechat-id-redacted]");
    }
    fflush(stdout);
    funlockfile(stdout);
    heap_caps_free(snapshot);
    result(ESP_OK);
}

static void room_snapshot(void)
{
    starter_room_snapshot_t *snapshot = heap_caps_calloc(
        1, sizeof(*snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snapshot) { result(ESP_ERR_NO_MEM); return; }
    bool ready = starter_runtime_room_read(0, snapshot);
    if (ready) {
        flockfile(stdout);
        printf("+ROOM:state=%u,assigned=%u,known=%u,open=%u,busy=%u,members=%u,online=%u,transmitting=%u,generation=%lu,revision=%lu\r\n",
               (unsigned)snapshot->state, (unsigned)snapshot->assigned,
               (unsigned)snapshot->assignment_known, (unsigned)snapshot->app_open,
               (unsigned)snapshot->busy, (unsigned)snapshot->member_count,
               (unsigned)snapshot->online_count, (unsigned)snapshot->transmitting,
               (unsigned long)snapshot->connection_generation, (unsigned long)snapshot->revision);
        fflush(stdout);
        funlockfile(stdout);
    }
    heap_caps_free(snapshot);
    result(ready ? ESP_OK : ESP_ERR_TIMEOUT);
}

bool starter_at_regression_command(char *line)
{
    if (!strcmp(line, "AT+DEVICE?")) {
        const esp_app_desc_t *app = esp_app_get_description();
        char sha[65], device[65];
        starter_runtime_copy_device_id(device, sizeof(device));
        for (unsigned i = 0; i < 32; ++i) snprintf(sha + i * 2, 3, "%02x", app->app_elf_sha256[i]);
        flockfile(stdout);
        printf("+DEVICE:project=%.32s,version=%.32s,idf=%.32s,elf=%s,id=%s\r\n",
               app->project_name, app->version, app->idf_ver, sha, device);
        fflush(stdout);
        funlockfile(stdout);
        result(ESP_OK);
    } else if (!strcmp(line, "AT+UI?")) result(starter_product_ui_inspect());
    else if (!strncmp(line, "AT+UI=POINTER,", 14)) {
        unsigned x, y, pressed;
        int consumed = 0;
        if (sscanf(line + 14, "%u,%u,%u%n", &x, &y, &pressed, &consumed) != 3 ||
            line[14 + consumed] != '\0' || x > UINT16_MAX || y > UINT16_MAX || pressed > 1)
            result(ESP_ERR_INVALID_ARG);
        else result(starter_product_ui_pointer((uint16_t)x, (uint16_t)y, pressed != 0));
    } else if (!strcmp(line, "AT+DOOR?")) result(starter_product_door_control(STARTER_PRODUCT_DOOR_STATUS));
    else if (!strcmp(line, "AT+DOOR=HOME")) result(starter_product_door_control(STARTER_PRODUCT_DOOR_HOME));
    else if (!strcmp(line, "AT+DOOR=NEXT")) result(starter_product_door_control(STARTER_PRODUCT_DOOR_NEXT));
    else if (!strcmp(line, "AT+DOOR=PREVIOUS")) result(starter_product_door_control(STARTER_PRODUCT_DOOR_PREVIOUS));
    else if (!strcmp(line, "AT+DOOR=VIEWER")) result(starter_product_door_control(STARTER_PRODUCT_DOOR_VIEWER));
    else if (!strcmp(line, "AT+DOOR=SENDER")) result(starter_product_door_control(STARTER_PRODUCT_DOOR_SENDER));
    else if (!strcmp(line, "AT+DOOR=DEVICES")) result(starter_product_door_control(STARTER_PRODUCT_DOOR_DEVICES));
    else if (!strcmp(line, "AT+DOOR=BACK")) result(starter_product_door_control(STARTER_PRODUCT_DOOR_BACK));
    else if (!strcmp(line, "AT+TXTRACE=START")) result(idle() ? starter_media_tx_timing_start() : ESP_ERR_INVALID_STATE);
    else if (!strcmp(line, "AT+TXTRACE=STOP")) result(starter_media_tx_timing_stop());
    else if (!strcmp(line, "AT+TXTRACE?")) result(idle() ? starter_media_tx_timing_dump() : ESP_ERR_INVALID_STATE);
    else if (!strcmp(line, "AT+CONTACTS?")) product_snapshot(true);
    else if (!strcmp(line, "AT+CALL?")) product_snapshot(false);
    else if (!strcmp(line, "AT+CONTACTS=REFRESH")) result(starter_runtime_contacts_refresh());
    else if (!strcmp(line, "AT+CALL=ACCEPT")) result(starter_runtime_call_accept());
    else if (!strcmp(line, "AT+CALL=REJECT")) result(starter_runtime_call_reject());
    else if (!strcmp(line, "AT+CALL=HANGUP")) result(starter_runtime_call_hangup());
    else if (!strcmp(line, "AT+CALL=MUTE,0") || !strcmp(line, "AT+CALL=MUTE,1"))
        result(starter_runtime_call_set_microphone_muted(line[13] == '1'));
    else if (!strcmp(line, "AT+CALL=CAMERA,0") || !strcmp(line, "AT+CALL=CAMERA,1"))
        result(starter_runtime_call_set_camera_enabled(starter_runtime_status().session_generation, line[15] == '1'));
    else if (!strncmp(line, "AT+CALL=", 8) || !strncmp(line, "AT+VIDCALL=", 11)) {
        uint8_t index;
        bool video = !strncmp(line, "AT+VIDCALL=", 11);
        if (!contact_index(line + (video ? 11 : 8), &index)) result(ESP_ERR_INVALID_ARG);
        else result(video ? starter_runtime_call_contact_video(index) : starter_runtime_call_contact(index));
    } else if (!strcmp(line, "AT+ROOM?")) room_snapshot();
    else if (!strcmp(line, "AT+ROOM=OPEN") || !strcmp(line, "AT+ROOM=CLOSE")) {
        starter_runtime_room_set_open(!strcmp(line, "AT+ROOM=OPEN"));
        result(ESP_OK);
    } else if (!strcmp(line, "AT+ROOM=REFRESH")) result(starter_runtime_room_refresh());
    else if (!strcmp(line, "AT+ROOM=LEAVE")) result(starter_runtime_room_leave());
    else if (!strcmp(line, "AT+ROOM=CREATE")) result(starter_runtime_room_create(""));
    else if (!strncmp(line, "AT+ROOM=CREATE,", 15)) result(starter_runtime_room_create(line + 15));
    else if (!strncmp(line, "AT+ROOM=JOIN,", 13)) {
        char *code = line + 13, *password = strchr(code, ',');
        if (password) *password++ = 0;
        result(starter_runtime_room_join(code, password ? password : ""));
    } else if (!strcmp(line, "AT+ROOM=PTT,0") || !strcmp(line, "AT+ROOM=PTT,1")) {
        bool pressed = line[12] == '1';
        bool accepted = starter_runtime_room_ptt(starter_runtime_status().connection_generation, pressed);
        result(!pressed || accepted ? ESP_OK : ESP_ERR_INVALID_STATE);
    } else if (!strcmp(line, "AT+NET?")) {
        printf("+NET:connected=%u,provisioning=%u,manual=%u,pending=%u,error=%s\r\n",
               (unsigned)wifi_manager_connected(), (unsigned)wifi_manager_provisioning(),
               (unsigned)wifi_manager_manually_disconnected(), (unsigned)wifi_manager_control_pending(),
               esp_err_to_name(wifi_manager_control_error()));
        result(ESP_OK);
    } else if (!strcmp(line, "AT+NET=DISCONNECT")) result(wifi_manager_disconnect());
    else if (!strcmp(line, "AT+REBOOT")) {
        if (!idle()) result(ESP_ERR_INVALID_STATE);
        else {
            result(ESP_OK);
            vTaskDelay(pdMS_TO_TICKS(150));
            esp_restart();
        }
    } else return false;
    return true;
}
