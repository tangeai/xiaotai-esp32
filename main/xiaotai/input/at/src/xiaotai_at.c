#include "xiaotai_at.h"
#include <stdio.h>
#include <string.h>

esp_err_t xiaotai_at_execute(const xiaotai_at_backend_t *backend, int argc, char **argv)
{
    if (!backend || !backend->line) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (argc < 1 || argc > 2 || !argv || !argv[0] || (argc == 2 && !argv[1])) goto done;
    if (!strcmp(argv[0], "AT") || !strcmp(argv[0], "AT+HELP")) {
        if (argc != 1) goto done;
        if (!strcmp(argv[0], "AT+HELP"))
            backend->line(backend->context, "AT | AT+HELP | AT+VERSION | AT+STATUS | AT+UI | AT+SCREEN | AT+AI START | AT+AI STOP");
        err = ESP_OK;
        goto done;
    }
    xiaotai_at_operation_t operation;
    if (!strcmp(argv[0], "AT+AI")) {
        if (argc != 2) goto done;
        if (!strcmp(argv[1], "START")) operation = XIAOTAI_AT_AI_START;
        else if (!strcmp(argv[1], "STOP")) operation = XIAOTAI_AT_AI_STOP;
        else goto done;
    } else {
        if (argc != 1) goto done;
        if (!strcmp(argv[0], "AT+VERSION")) operation = XIAOTAI_AT_VERSION;
        else if (!strcmp(argv[0], "AT+STATUS")) operation = XIAOTAI_AT_STATUS;
        else if (!strcmp(argv[0], "AT+UI")) operation = XIAOTAI_AT_UI;
        else if (!strcmp(argv[0], "AT+SCREEN")) operation = XIAOTAI_AT_SCREEN;
        else { err = ESP_ERR_NOT_SUPPORTED; goto done; }
    }
    err = backend->execute ? backend->execute(backend->context, operation) : ESP_ERR_NOT_SUPPORTED;
done:
    if (err == ESP_OK) backend->line(backend->context, "OK");
    else {
        char result[80];
        snprintf(result, sizeof(result), "ERROR: %s", esp_err_to_name(err));
        backend->line(backend->context, result);
    }
    return err;
}
