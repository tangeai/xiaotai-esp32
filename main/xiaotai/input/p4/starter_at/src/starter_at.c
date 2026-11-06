#include "starter_at.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "starter_product.h"
#include "wifi_manager.h"

#define AT_RX_BYTES 512
#define AT_LINE_BYTES 128
#define AT_STACK_BYTES 6144

static const char *TAG = "starter_at";
static TaskHandle_t s_task;
static bool s_commands_enabled = true;
static atomic_bool s_c6_ready;

void starter_at_c6_ready(void)
{
    atomic_store(&s_c6_ready, true);
}

static void at_reply(const char *reply)
{
    flockfile(stdout);
    printf("%s\r\n", reply);
    fflush(stdout);
    funlockfile(stdout);
}

static void at_command(char *line)
{
    if (strcmp(line, "AT") == 0) {
        at_reply("OK");
        return;
    }
    if (strcmp(line, "AT+CLI?") == 0) {
        at_reply(s_commands_enabled ? "+CLI:ON" : "+CLI:OFF");
        at_reply("OK");
        return;
    }
    if (strcmp(line, "AT+CLI=ON") == 0 || strcmp(line, "AT+CLI=OFF") == 0) {
        s_commands_enabled = strcmp(line, "AT+CLI=ON") == 0;
        at_reply("OK");
        return;
    }
    if (!s_commands_enabled) {
        at_reply("ERROR,CLI_DISABLED");
        return;
    }
    if (strcmp(line, "AT+HELP") == 0) {
        at_reply("+HELP:AT|AT+CLI?|AT+CLI=ON|OFF|AT+WIFI?|AT+WIFI=<ssid>,<password>|AT+C6RETRY");
        at_reply("OK");
        return;
    }
    if (strcmp(line, "AT+C6RETRY") == 0) {
        if (!starter_product_request_c6_retry()) {
            at_reply("ERROR,C6_RETRY_UNAVAILABLE");
            return;
        }
        at_reply("OK");
        return;
    }
    if (strcmp(line, "AT+WIFI?") == 0) {
        wifi_manager_credentials_t saved = {0};
        esp_err_t err = wifi_manager_load_credentials(&saved);
        if (err == ESP_OK) {
            flockfile(stdout);
            printf("+WIFI:ssid=%s,connected=%u\r\n", saved.ssid,
                   (unsigned)wifi_manager_connected());
            fflush(stdout);
            funlockfile(stdout);
            at_reply("OK");
        } else {
            at_reply("ERROR,WIFI_READ_FAILED");
        }
        return;
    }
    const char *prefix = "AT+WIFI=";
    if (strncmp(line, prefix, strlen(prefix)) == 0) {
        if (!atomic_load(&s_c6_ready)) {
            at_reply("ERROR,C6_NOT_READY");
            return;
        }
        char *ssid = line + strlen(prefix);
        char *password = strchr(ssid, ',');
        if (password == NULL) {
            at_reply("ERROR,WIFI_FORMAT");
            return;
        }
        *password++ = '\0';
        if (!wifi_manager_credentials_valid(ssid, password, NULL, 0)) {
            at_reply("ERROR,WIFI_INVALID");
            return;
        }
        esp_err_t err = wifi_manager_save_credentials(ssid, password);
        if (err != ESP_OK) {
            at_reply("ERROR,WIFI_SAVE_FAILED");
            return;
        }
        at_reply("+WIFI:SAVED,RESTARTING");
        at_reply("OK");
        vTaskDelay(pdMS_TO_TICKS(150));
        esp_restart();
        return;
    }
    at_reply("ERROR,UNKNOWN_COMMAND");
}

static void at_task(void *argument)
{
    (void)argument;
    char line[AT_LINE_BYTES] = {0};
    uint8_t bytes[64];
    size_t length = 0;
    bool overflow = false;
    at_reply("+READY:AT,UART0,115200");
    for (;;) {
        int count = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, bytes,
                                    sizeof(bytes), pdMS_TO_TICKS(50));
        for (int i = 0; i < count; ++i) {
            uint8_t value = bytes[i];
            if (value == '\r' || value == '\n') {
                if (overflow) at_reply("ERROR,LINE_TOO_LONG");
                else if (length != 0) {
                    line[length] = '\0';
                    at_command(line);
                }
                memset(line, 0, sizeof(line));
                length = 0;
                overflow = false;
            } else if (value == 0x08 || value == 0x7f) {
                if (!overflow && length != 0) line[--length] = '\0';
            } else if (!overflow) {
                if (value < 0x20 || length + 1 >= sizeof(line)) {
                    overflow = true;
                    length = 0;
                } else {
                    line[length++] = (char)value;
                }
            }
        }
    }
}

esp_err_t starter_at_start(void)
{
    if (s_task != NULL) return ESP_OK;
    if (!uart_is_driver_installed(CONFIG_ESP_CONSOLE_UART_NUM)) {
        esp_err_t err = uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM,
                                            AT_RX_BYTES, 0, 0, NULL, 0);
        if (err != ESP_OK) return err;
        const uart_config_t config = {
            .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        err = uart_param_config(CONFIG_ESP_CONSOLE_UART_NUM, &config);
        if (err != ESP_OK) return err;
    }
    ESP_RETURN_ON_ERROR(uart_flush_input(CONFIG_ESP_CONSOLE_UART_NUM), TAG,
                        "flush UART input");
    if (xTaskCreateWithCaps(at_task, "starter_at", AT_STACK_BYTES, NULL, 3,
                            &s_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
