#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XIAOTAI_AT_VERSION,
    XIAOTAI_AT_STATUS,
    XIAOTAI_AT_UI,
    XIAOTAI_AT_SCREEN,
    XIAOTAI_AT_AI_START,
    XIAOTAI_AT_AI_STOP,
} xiaotai_at_operation_t;

typedef struct {
    void *context;
    /* Missing hardware/operations must return ESP_ERR_NOT_SUPPORTED. */
    esp_err_t (*execute)(void *context, xiaotai_at_operation_t operation);
    void (*line)(void *context, const char *text);
} xiaotai_at_backend_t;

/* argv comes from the transport tokenizer. No hardware or business state here.
 * One final OK/ERROR per command; OK on actions means accepted, not completed. */
esp_err_t xiaotai_at_execute(const xiaotai_at_backend_t *backend, int argc, char **argv);
#ifdef __cplusplus
}
#endif
