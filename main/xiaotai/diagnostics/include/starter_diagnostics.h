#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Read-only aggregation. Each owner retains its state and failure semantics. */
void starter_diagnostics_identity(void);
void starter_diagnostics_status(void);
void starter_diagnostics_wake(void);
esp_err_t starter_diagnostics_version(void);
esp_err_t starter_diagnostics_ui(void);
esp_err_t starter_diagnostics_screen(void);

#ifdef __cplusplus
}
#endif
