#include "wifi_page_model.h"

#include <stdio.h>
#include <string.h>

#define DISPLAY_WIFI_COLOR_MUTED    0x48656F
#define DISPLAY_WIFI_COLOR_INFO     0x52708B
#define DISPLAY_WIFI_COLOR_SUCCESS  0x2E8F6B

static void wifi_page_model_copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    (void)snprintf(dst, dst_size, "%s", src);
}

void wifi_page_model_build(const display_status_t *status,
                                            wifi_page_model_t *model)
{
    if (model == NULL) {
        return;
    }

    memset(model, 0, sizeof(*model));
    if (status == NULL) {
        model->connection_color = DISPLAY_WIFI_COLOR_MUTED;
        model->scan_color = DISPLAY_WIFI_COLOR_MUTED;
        wifi_page_model_copy_text(model->connection_text,
                                  sizeof(model->connection_text),
                                  "WiFi offline");
        wifi_page_model_copy_text(model->scan_text,
                                  sizeof(model->scan_text),
                                  "0 APs");
        return;
    }

    if (status->network_connected) {
        model->connection_color = DISPLAY_WIFI_COLOR_SUCCESS;
        if (status->network_ssid[0] != '\0') {
            (void)snprintf(model->connection_text,
                           sizeof(model->connection_text),
                           "Connected to %s",
                           status->network_ssid);
        } else {
            wifi_page_model_copy_text(model->connection_text,
                                                       sizeof(model->connection_text),
                                                       "Connected");
        }
    } else {
        model->connection_color = DISPLAY_WIFI_COLOR_MUTED;
        wifi_page_model_copy_text(model->connection_text,
                                                   sizeof(model->connection_text),
                                                   "WiFi offline");
    }

    model->visible_count = status->wifi_scan_count < DISPLAY_WIFI_SCAN_MAX
                               ? status->wifi_scan_count
                               : DISPLAY_WIFI_SCAN_MAX;

    if (status->wifi_scan_in_progress) {
        model->scan_color = DISPLAY_WIFI_COLOR_INFO;
        wifi_page_model_copy_text(model->scan_text,
                                                   sizeof(model->scan_text),
                                                   "Scanning");
    } else {
        model->scan_color = model->visible_count > 0
                                ? DISPLAY_WIFI_COLOR_SUCCESS
                                : DISPLAY_WIFI_COLOR_MUTED;
        (void)snprintf(model->scan_text,
                       sizeof(model->scan_text),
                       "%u APs",
                       (unsigned)status->wifi_scan_count);
    }

    for (uint16_t index = 0; index < model->visible_count; ++index) {
        const display_wifi_scan_result_t *result = &status->wifi_scan_results[index];
        bool connected = status->network_connected && strcmp(status->network_ssid, result->ssid) == 0;

        model->rows[index].visible = true;
        model->rows[index].connected = connected;
        if (connected) {
            (void)snprintf(model->rows[index].text,
                           sizeof(model->rows[index].text),
                           "%s  Connected",
                           result->ssid);
        } else {
            wifi_page_model_copy_text(model->rows[index].text,
                                                       sizeof(model->rows[index].text),
                                                       result->ssid);
        }
    }
}

bool wifi_page_model_equals(const wifi_page_model_t *lhs,
                                             const wifi_page_model_t *rhs)
{
    if (lhs == rhs) {
        return true;
    }

    if (lhs == NULL || rhs == NULL) {
        return false;
    }

    if (lhs->connection_color != rhs->connection_color ||
        lhs->scan_color != rhs->scan_color ||
        lhs->visible_count != rhs->visible_count ||
        strcmp(lhs->connection_text, rhs->connection_text) != 0 ||
        strcmp(lhs->scan_text, rhs->scan_text) != 0) {
        return false;
    }

    for (uint16_t index = 0; index < DISPLAY_WIFI_SCAN_MAX; ++index) {
        if (lhs->rows[index].visible != rhs->rows[index].visible ||
            lhs->rows[index].connected != rhs->rows[index].connected ||
            strcmp(lhs->rows[index].text, rhs->rows[index].text) != 0) {
            return false;
        }
    }

    return true;
}
