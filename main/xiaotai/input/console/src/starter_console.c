/*
 * 开发期串口控制台。
 *
 * 这个模块是人工操作的 adapter：只解析参数、调用公开接口并打印结果。
 * 它不拥有 Wi-Fi、凭证或会话状态。量产按键/UI 应调用相同接口，而不是复用
 * ESP console 的命令处理函数。
 */
#include "starter_console.h"
#include "starter_diagnostics.h"
#include "xiaotai_at.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform_client.h"
#include "runtime_config.h"
#include "starter_media.h"
#include "starter_product.h"
#include "starter_runtime.h"
#include "starter_tirtc.h"
#include "starter_voice.h"
#include "wifi_manager.h"

static esp_console_repl_t *s_repl;

static void at_line(void *context, const char *line)
{
    (void)context;
    puts(line);
}

static esp_err_t console_screen(void)
{
    /* Bulk output is background work: it must not outrank either idle task.
     * The UI owner still renders at its own priority; restore REPL priority
     * on every result so control commands retain their original scheduling. */
    UBaseType_t priority = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, tskIDLE_PRIORITY);
    esp_err_t err = starter_diagnostics_screen();
    vTaskPrioritySet(NULL, priority);
    return err;
}

static esp_err_t at_operation(void *context, xiaotai_at_operation_t operation)
{
    (void)context;
    switch (operation) {
    case XIAOTAI_AT_VERSION: return starter_diagnostics_version();
    case XIAOTAI_AT_STATUS: starter_diagnostics_status(); return ESP_OK;
    case XIAOTAI_AT_UI: return starter_diagnostics_ui();
    case XIAOTAI_AT_SCREEN: return console_screen();
    case XIAOTAI_AT_AI_START: return starter_runtime_ai_start();
    case XIAOTAI_AT_AI_STOP: return starter_runtime_ai_stop();
    default: return ESP_ERR_NOT_SUPPORTED;
    }
}

static int command_at(int argc, char **argv)
{
    const xiaotai_at_backend_t backend = {.execute = at_operation, .line = at_line};
    return xiaotai_at_execute(&backend, argc, argv) == ESP_OK ? 0 : 1;
}

static void print_firmware_identity(void)
{
    starter_diagnostics_identity();
}

static int command_version(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("usage: version\n");
        return 1;
    }
    print_firmware_identity();
    return 0;
}

/* [DEBUG-wake49] Console-only output: never print from the capture/model hot path. */
static void print_wake_diagnostics(void)
{
    starter_diagnostics_wake();
}

static int command_voice_diag(int argc, char **argv)
{
    char *end = NULL;
    unsigned long seconds = argc == 1 ? 20 : argc == 2 ? strtoul(argv[1], &end, 10) : 0;
    if (seconds < 5 || seconds > 30 || (argc == 2 && (end == argv[1] || *end != '\0'))) {
        printf("Usage: voice-diag [seconds:5..30]\n");
        return 1;
    }
    print_firmware_identity();
    printf("Voicute observation only: say 你好小钛; no configuration changes.\n");
    for (unsigned long i = 0; i <= seconds; ++i) {
        print_wake_diagnostics();
        if (i != seconds) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    printf("[DEBUG-wake49] Observation complete; no configuration or model state changed.\n");
    return 0;
}

/* 汇总各深模块的只读快照，不读取或打印 device_secret。 */
static int command_status(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) return 1;
    starter_diagnostics_status();
    return 0;
}

static int command_ai_start(int argc, char **argv)
{
    /* 串口命令和板载 BOOT 按键复用同一产品控制接口。 */
    (void)argv;
    if (argc != 1) {
        printf("usage: ai-start\n");
        return 1;
    }
    esp_err_t err = starter_runtime_ai_start();
    if (err != ESP_OK) {
        printf("AI start rejected: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

static int command_voice_config(int argc, char **argv)
{
    char *end = NULL;
    unsigned long threshold = argc == 2 ? strtoul(argv[1], &end, 10) : 0;
    if (argc != 2 || end == argv[1] || *end != '\0' || threshold < 50 || threshold > 990) {
        printf("usage: voice-config <threshold-milli:50..990>; volatile, default 700\n");
        return 1;
    }
    esp_err_t err = starter_voice_configure((unsigned)threshold, true);
    printf("Voicute configuration: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int command_ai_stop(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("usage: ai-stop\n");
        return 1;
    }
    esp_err_t err = starter_runtime_ai_stop();
    if (err != ESP_OK) {
        printf("AI stop rejected: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

static int command_voice_test(int argc, char **argv)
{
    static const struct {
        const char *phrase;
    } cases[] = {
        {"你好小钛"},
        {"小钛小钛，今天天气怎么样"},
        {"小钛同学，给我讲个故事"},
        {"你好小钛，帮我联系家人"},
    };
    if (argc == 2 && strcmp(argv[1], "self-test") == 0) {
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            starter_voice_result_t parsed = {0};
            esp_err_t err = starter_voice_parse(cases[i].phrase, &parsed);
            if (err != ESP_OK || parsed.intent != STARTER_VOICE_INTENT_WAKE_ONLY) {
                printf("voice parser self-test failed at case %u: err=%s intent=%s\n",
                       (unsigned)i, esp_err_to_name(err),
                       starter_voice_intent_name(parsed.intent));
                return 1;
            }
        }
        printf("voice parser self-test passed: %u wake suffixes map to AI\n",
               (unsigned)(sizeof(cases) / sizeof(cases[0])));
        return 0;
    }

    starter_voice_result_t result = {0};
    if (argc == 2) {
        /* 中文输入在支持 UTF-8 argv 的控制台实现上仍可直接使用。 */
        (void)starter_voice_parse(argv[1], &result);
    }
    if (result.intent == STARTER_VOICE_INTENT_NONE) {
        printf("usage: voice-test self-test|你好小钛[，任意后续内容]\n");
        return 1;
    }
    esp_err_t err = starter_product_submit_voice(&result);
    if (err != ESP_OK) {
        printf("voice intent submission failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("voice intent queued: %s\n", starter_voice_intent_name(result.intent));
    return 0;
}

static int command_wifi_set(int argc, char **argv)
{
    /* 保存后重启，让 wifi_manager 从单一的正常启动路径应用新配置。 */
    if (argc != 3) {
        printf("usage: wifi-set <ssid> <password>; use \"\" for an open network\n");
        return 1;
    }
    char error[80];
    if (!wifi_manager_credentials_valid(argv[1], argv[2], error, sizeof(error))) {
        printf("invalid Wi-Fi config: %s\n", error);
        return 1;
    }
    esp_err_t err = wifi_manager_save_credentials(argv[1], argv[2]);
    if (err != ESP_OK) {
        printf("save failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Wi-Fi config saved; restarting...\n");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return 0;
}

static int command_wifi_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t err = wifi_manager_forget_credentials();
    if (err != ESP_OK) {
        printf("clear failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Wi-Fi config cleared; restarting into SoftAP mode...\n");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return 0;
}

static int command_tirtc_set(int argc, char **argv)
{
    /* 仅用于受控联调；正常设备通过验证码绑定获得并保存凭证。 */
    if (argc < 3 || argc > 4) {
        printf("usage: tirtc-set <device_id> <device_secret> [client_id]\n");
        return 1;
    }
    runtime_tirtc_config_t config = {0};
    if (strlen(argv[1]) >= sizeof(config.device_id) ||
        strlen(argv[2]) >= sizeof(config.device_secret) ||
        (argc == 4 && strlen(argv[3]) >= sizeof(config.client_id))) {
        printf("one or more arguments are too long\n");
        return 1;
    }
    (void)snprintf(config.device_id, sizeof(config.device_id), "%s", argv[1]);
    (void)snprintf(config.device_secret, sizeof(config.device_secret), "%s", argv[2]);
    if (argc == 4) {
        (void)snprintf(config.client_id, sizeof(config.client_id), "%s", argv[3]);
    }
    esp_err_t err = runtime_config_save_tirtc(&config);
    if (err != ESP_OK) {
        printf("save failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("device credentials saved (secret is not displayed); restarting...\n");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return 0;
}

static int command_tirtc_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t err = runtime_config_clear_tirtc();
    if (err != ESP_OK) {
        printf("clear failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("device credentials cleared; restarting into binding flow...\n");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return 0;
}

static int command_restart(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("restarting...\n");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;
}

static esp_err_t register_command(const char *name,
                                  const char *help,
                                  esp_console_cmd_func_t function)
{
    const esp_console_cmd_t command = {
        .command = name,
        .help = help,
        .func = function,
    };
    return esp_console_cmd_register(&command);
}

esp_err_t starter_console_start(void)
{
    if (s_repl != NULL) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_console_register_help_command(),
                        "starter_console",
                        "register help");
    const char *at_commands[] = {"AT", "AT+HELP", "AT+VERSION", "AT+STATUS", "AT+UI", "AT+SCREEN", "AT+AI"};
    for (size_t i = 0; i < sizeof(at_commands) / sizeof(at_commands[0]); ++i) {
        ESP_RETURN_ON_ERROR(register_command(at_commands[i], "Local diagnostic AT command; see AT+HELP", command_at),
                            "starter_console", "register AT command");
    }
    ESP_RETURN_ON_ERROR(register_command("version", "Show firmware identity", command_version),
                        "starter_console", "register version");
    ESP_RETURN_ON_ERROR(register_command("status", "Show starter status", command_status),
                        "starter_console", "register status");
    ESP_RETURN_ON_ERROR(register_command("voice-config", "Set volatile Voicute threshold (50..990)", command_voice_config),
                        "starter_console", "register voice-config");
    ESP_RETURN_ON_ERROR(register_command("voice-diag", "Observe audio levels for 5..30 seconds", command_voice_diag),
                        "starter_console", "register voice-diag");
    ESP_RETURN_ON_ERROR(register_command("ai-start", "Start AI talk", command_ai_start),
                        "starter_console", "register ai-start");
    ESP_RETURN_ON_ERROR(register_command("ai-stop", "Stop AI talk", command_ai_stop),
                        "starter_console", "register ai-stop");
    ESP_RETURN_ON_ERROR(register_command(
                            "voice-test",
                            "Manual text injection only; not acoustic recognition",
                            command_voice_test),
                        "starter_console", "register voice-test");
    ESP_RETURN_ON_ERROR(register_command("wifi-set", "wifi-set <ssid> <password>", command_wifi_set),
                        "starter_console", "register wifi-set");
    ESP_RETURN_ON_ERROR(register_command("wifi-clear", "Clear Wi-Fi and use SoftAP", command_wifi_clear),
                        "starter_console", "register wifi-clear");
    ESP_RETURN_ON_ERROR(register_command("tirtc-set", "Pre-load device credentials", command_tirtc_set),
                        "starter_console", "register tirtc-set");
    ESP_RETURN_ON_ERROR(register_command("tirtc-clear", "Clear device credentials", command_tirtc_clear),
                        "starter_console", "register tirtc-clear");
    ESP_RETURN_ON_ERROR(register_command("restart", "Restart the device", command_restart),
                        "starter_console", "register restart");

    /* sdkconfig.defaults 默认走 USB Serial/JTAG，UART 分支便于其他板型复用。 */
    esp_console_repl_config_t repl = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl.prompt = "xiaotai> ";
    repl.max_cmdline_length = 512;
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t device =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_usb_serial_jtag(&device, &repl, &s_repl),
                        "starter_console", "create USB Serial/JTAG REPL");
#elif defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t device = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_uart(&device, &repl, &s_repl),
                        "starter_console", "create UART REPL");
#else
#error Unsupported console transport
#endif
    return esp_console_start_repl(s_repl);
}
