// SPDX-License-Identifier: MIT
#pragma once
#include "xiaotai/port/mqtt.h"

/* Private P-4 interfaces; the worker owns lifecycle, the MQTT port invokes
 * receive/disconnect serially, and the owner runs subscribers. */
xiaotai_err_t platform_signals_start(void);
void platform_signals_reset(void);
void platform_signals_disconnected(void);
void platform_signals_message(xiaotai_mqtt_t *client, const xiaotai_mqtt_event_t *event,
                              const char *cmd, const char *notify, const char *ack,
                              uint32_t generation, bool authorized);
uint32_t platform_signals_step(void);
bool platform_signals_binding_pending(void); /* an accepted unbind is still owned by the signal pool */
typedef void (*platform_signal_binding_fn)(bool confirmed, int32_t ack_id, uint32_t generation, void *ctx);
xiaotai_err_t platform_signals_binding_register(platform_signal_binding_fn fn, void *ctx);
